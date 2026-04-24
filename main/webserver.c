#include "webserver.h"
#include "config.h"
// WS2812 boards define WS2812_NUM_LEDS; simple LED boards define LED_GPIO
#ifdef WS2812_NUM_LEDS
#include "ws2812.h"
#include "dog_peripherals.h"
#else
#include "led.h"
#endif
#include "servo.h"
#include "timekeep.h"
#ifndef DISABLE_OTA
#include "ota.h"
#endif
#include <string.h>

// rf.h is only present when board has an RF module
#ifdef RF_RX_GPIO
#include "rf.h"
#endif
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "cJSON.h"

static httpd_handle_t s_server = NULL;

// ══════════════════════════════════════════════════════════════
//  Named-action dispatcher (used by web handlers + scheduler)
// ══════════════════════════════════════════════════════════════
void execute_named_action(const char *action) {
    if      (strcmp(action, "s1on")   == 0) servo_quick_action(1, POS1_ON,  POS1_NEUTRAL);
    else if (strcmp(action, "s1off")  == 0) servo_quick_action(1, POS1_OFF, POS1_NEUTRAL);
    else if (strcmp(action, "s2on")   == 0) servo_quick_action(2, POS2_ON,  POS2_NEUTRAL);
    else if (strcmp(action, "s2off")  == 0) servo_quick_action(2, POS2_OFF, POS2_NEUTRAL);
#if SERVO_COUNT >= 3
    else if (strcmp(action, "s3on")   == 0) servo_quick_action(3, POS3_ON,  POS3_NEUTRAL);
    else if (strcmp(action, "s3off")  == 0) servo_quick_action(3, POS3_OFF, POS3_NEUTRAL);
#endif
#if SERVO_COUNT >= 4
    else if (strcmp(action, "s4on")   == 0) servo_quick_action(4, POS4_ON,  POS4_NEUTRAL);
    else if (strcmp(action, "s4off")  == 0) servo_quick_action(4, POS4_OFF, POS4_NEUTRAL);
#endif
    else if (strcmp(action, "l1on")   == 0) led_action_set(true);
    else if (strcmp(action, "l1off")  == 0) led_action_set(false);
    else if (strcmp(action, "toggle") == 0) led_action_toggle();
    else if (strcmp(action, "hi")     == 0) servo_quick_action(1, 40, POS1_NEUTRAL);
    else if (strcmp(action, "lay")    == 0) {
        servo_action_set(1, 0); servo_action_set(2, 0);
        servo_action_set(3, 180); servo_action_set(4, 180);
    }
    else if (strcmp(action, "stand")  == 0) {
        servo_action_set(1, POS1_NEUTRAL); servo_action_set(2, POS2_NEUTRAL);
        servo_action_set(3, POS3_NEUTRAL); servo_action_set(4, POS4_NEUTRAL);
    }
    else if (strcmp(action, "walk_fwd") == 0) {
        servo_quick_action(1, 60, POS1_NEUTRAL);
        servo_quick_action(4, 120, POS4_NEUTRAL);
    }
    else if (strcmp(action, "walk_bwd") == 0) {
        servo_quick_action(2, 120, POS2_NEUTRAL);
        servo_quick_action(3, 60, POS3_NEUTRAL);
    }
    else ESP_LOGW("ACTION", "Unknown action: %s", action);
}

// ══════════════════════════════════════════════════════════════
//  Handlers
// ══════════════════════════════════════════════════════════════

static esp_err_t cors_options_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    const char html[] =
        "<!DOCTYPE html><html><head><title>MOJ ESP32 Control v5 (Touch Fix)</title>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0;}"
        "body{font-family:'Segoe UI',sans-serif;background:#1a1a2e;color:#e0e0e0;padding:20px;}"
        "h1{color:#00d4ff;margin-bottom:10px;}"
        ".time{font-size:28px;color:#00ff88;font-family:monospace;margin:10px 0;}"
        ".epoch{font-size:13px;color:#888;margin-bottom:16px;}"
        "h2{color:#aaa;margin:16px 0 8px;font-size:15px;text-transform:uppercase;letter-spacing:1px;}"
        "button{font-size:16px;margin:3px;padding:8px 18px;border:none;border-radius:6px;"
        "cursor:pointer;background:#2a2a4a;color:#fff;transition:background .2s;}"
        "button:hover{background:#3a3a6a;} button:active{background:#00d4ff;color:#000;}"
        "input{padding:8px;border-radius:6px;border:1px solid #444;background:#2a2a4a;color:#fff;margin:3px;}"
        ".section{background:#16213e;padding:14px;border-radius:10px;margin:10px 0;}"
        ".ver{color:#555;font-size:12px;margin-top:20px;}"
        "a{color:#00d4ff;}"
        "</style></head><body>"
        "<h1>MOJ ESP32 Control v5 (Touch Fix)</h1>"
        "<div class='time' id='clock'>--:--:--</div>"
        "<div class='epoch'>Epoch: <span id='epoch'>-</span> | Synced: <span id='synced'>no</span></div>"

        "<div class='section'><h2>Dog Animations</h2>"
        "<button onclick=\"f('/hi')\">Say Hi</button>"
        "<button onclick=\"f('/lay')\">Lay Down</button>"
        "<button onclick=\"f('/stand')\">Stand Up</button>"
        "<button onclick=\"f('/walk_fwd')\">Walk Forward</button>"
        "<button onclick=\"f('/walk_bwd')\">Walk Backward</button></div>"
        
        "<div class='section'><h2>Text to Speech</h2>"
        "<input id='say' placeholder='What should I say?' style='width:200px'>"
        "<button onclick=\"say()\">Speak</button></div>"
        
        "<div class='section'><h2>LED</h2>"
        "<button onclick=\"fetch('/led?state=toggle')\">Toggle LED</button></div>"

        "<div class='section'><h2>Servo 1</h2>"
        "<button onclick=\"f('/s1on')\">On</button>"
        "<button onclick=\"f('/s1off')\">Off</button>"
        "<button onclick=\"f('/servo?num=1&angle=0')\">0&deg;</button>"
        "<button onclick=\"f('/servo?num=1&angle=90')\">90&deg;</button>"
        "<button onclick=\"f('/servo?num=1&angle=180')\">180&deg;</button></div>"

        "<div class='section'><h2>Servo 2</h2>"
        "<button onclick=\"f('/s2on')\">On</button>"
        "<button onclick=\"f('/s2off')\">Off</button>"
        "<button onclick=\"f('/servo?num=2&angle=0')\">0&deg;</button>"
        "<button onclick=\"f('/servo?num=2&angle=90')\">90&deg;</button>"
        "<button onclick=\"f('/servo?num=2&angle=180')\">180&deg;</button></div>"

#if SERVO_COUNT >= 3
        "<div class='section'><h2>Servo 3 (BL)</h2>"
        "<button onclick=\"f('/s3on')\">On</button>"
        "<button onclick=\"f('/s3off')\">Off</button>"
        "<button onclick=\"f('/servo?num=3&angle=0')\">0&deg;</button>"
        "<button onclick=\"f('/servo?num=3&angle=90')\">90&deg;</button>"
        "<button onclick=\"f('/servo?num=3&angle=180')\">180&deg;</button></div>"
#endif
#if SERVO_COUNT >= 4
        "<div class='section'><h2>Servo 4 (BR)</h2>"
        "<button onclick=\"f('/s4on')\">On</button>"
        "<button onclick=\"f('/s4off')\">Off</button>"
        "<button onclick=\"f('/servo?num=4&angle=0')\">0&deg;</button>"
        "<button onclick=\"f('/servo?num=4&angle=90')\">90&deg;</button>"
        "<button onclick=\"f('/servo?num=4&angle=180')\">180&deg;</button></div>"
#endif

#ifdef RF_RX_GPIO
        "<div class='section'><h2>Send RF Code</h2>"
        "<input id='code' placeholder='hex e.g. 123456'>"
        "<button onclick=\"f('/send?code='+document.getElementById('code').value)\">Send</button></div>"
#endif // RF_RX_GPIO

        "<div class='section'><h2>Schedule Action</h2>"
        "<input id='sa' placeholder='action e.g. s1on' style='width:120px'>"
        "<input id='sd' placeholder='delay (sec)' type='number' style='width:100px'>"
        "<button onclick=\"f('/schedule?action='+document.getElementById('sa').value"
        "+'&delay='+document.getElementById('sd').value)\">Schedule</button></div>"

#ifndef DISABLE_OTA
        "<div class='ver'>v" FW_VERSION " | <a href='/ota'>OTA Update</a></div>"
#else
        "<div class='ver'>v" FW_VERSION "</div>"
#endif

        "<script>"
        "function f(u){fetch(u)}"
        "async function say(){"
        "let t=document.getElementById('say').value;"
        "if(!t)return;"
        "try{"
        "let u='https://api.codetabs.com/v1/proxy?quest='+encodeURIComponent('https://translate.google.com/translate_tts?ie=UTF-8&q='+encodeURIComponent(t)+'&tl=en&client=tw-ob');"
        "let r=await fetch(u);"
        "let ab=await r.arrayBuffer();"
        "let ctx=new (window.AudioContext||window.webkitAudioContext)();"
        "let dec=await ctx.decodeAudioData(ab);"
        "let off=new OfflineAudioContext(1,dec.duration*16000,16000);"
        "let src=off.createBufferSource();"
        "src.buffer=dec;src.connect(off.destination);src.start();"
        "let ren=await off.startRendering();"
        "let dat=ren.getChannelData(0);"
        "let pcm=new Int16Array(dat.length);"
        "for(let i=0;i<dat.length;i++){"
        "let s=Math.max(-1,Math.min(1,dat[i]));"
        "pcm[i]=s<0?s*0x8000:s*0x7FFF;"
        "}"
        "fetch('/audio',{method:'POST',body:new Blob([pcm.buffer])});"
        "}catch(e){alert('TTS err: '+e);}"
        "}"
        "setInterval(function(){"
        "fetch('/time').then(r=>r.json()).then(d=>{"
        "document.getElementById('clock').textContent=d.formatted;"
        "document.getElementById('epoch').textContent=d.epoch;"
        "document.getElementById('synced').textContent=d.synced?'yes':'no';"
        "})},1000);"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t led_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "state", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "toggle") == 0) led_action_toggle();
            else if (strcmp(param, "on") == 0)  led_action_set(true);
            else if (strcmp(param, "off") == 0) led_action_set(false);
        }
    }
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t servo_handler(httpd_req_t *req) {
    char buf[100];
    int servo = 0, angle = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[16];
        if (httpd_query_key_value(buf, "num", p, sizeof(p)) == ESP_OK)   servo = atoi(p);
        if (httpd_query_key_value(buf, "angle", p, sizeof(p)) == ESP_OK) angle = atoi(p);
    }
    if (servo >= 1 && servo <= servo_count()) servo_action_set(servo, angle);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

#ifdef RF_RX_GPIO
static esp_err_t send_rf_handler(httpd_req_t *req) {
    char buf[100];
    uint32_t code = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[32];
        if (httpd_query_key_value(buf, "code", p, sizeof(p)) == ESP_OK)
            code = strtoul(p, NULL, 16);
    }
    if (code) {
        rf_send_code(code, 24);
        ESP_LOGI("WEB", "Sent RF 0x%06lX", (unsigned long)code);
    }
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
#endif // RF_RX_GPIO

static esp_err_t time_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    char buf[32];
    timekeep_format(buf, sizeof(buf));
    cJSON_AddStringToObject(root, "formatted", buf);
    cJSON_AddNumberToObject(root, "epoch", (double)timekeep_now());
    cJSON_AddBoolToObject(root, "synced", timekeep_is_synced());
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "running");
    cJSON_AddStringToObject(root, "version", FW_VERSION);
    cJSON_AddNumberToObject(root, "epoch", (double)timekeep_now());
    cJSON_AddBoolToObject(root, "time_synced", timekeep_is_synced());
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// Quick-action: now async — returns immediately, servo worker does the move
static esp_err_t quick_action_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    // Strip leading '/' and dispatch
    if (uri[0] == '/') uri++;
    execute_named_action(uri);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t schedule_handler(httpd_req_t *req) {
    char buf[128];
    char action[16] = {0};
    int delay_sec = 0;
    time_t at = 0;

    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char p[32];
        if (httpd_query_key_value(buf, "action", p, sizeof(p)) == ESP_OK)
            strncpy(action, p, sizeof(action) - 1);
        if (httpd_query_key_value(buf, "delay", p, sizeof(p)) == ESP_OK)
            delay_sec = atoi(p);
        if (httpd_query_key_value(buf, "at", p, sizeof(p)) == ESP_OK)
            at = (time_t)strtol(p, NULL, 10);
    }

    if (action[0]) {
        if (at > 0) {
            timekeep_schedule(action, at);
        } else if (delay_sec > 0) {
            timekeep_schedule(action, timekeep_now() + delay_sec);
        }
        httpd_resp_send(req, "Scheduled", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "Missing ?action=", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

#ifdef WS2812_NUM_LEDS
static esp_err_t audio_post_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    int remaining = req->content_len;
    if (remaining <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    // Stream the body in small chunks through the async queue.
    // Each chunk is a small malloc (≤4KB) — no large allocation needed.
    while (remaining > 0) {
        int chunk_sz = remaining < 4096 ? remaining : 4096;
        uint8_t *chunk = malloc(chunk_sz);
        if (!chunk) {
            ESP_LOGE("WEB", "No heap for %d-byte audio chunk", chunk_sz);
            // Drain the rest of the HTTP body so the connection stays clean
            char drain[512];
            while (remaining > 0) {
                int n = httpd_req_recv(req, drain, remaining < (int)sizeof(drain) ? remaining : (int)sizeof(drain));
                if (n <= 0) break;
                remaining -= n;
            }
            break;
        }

        // Read exactly chunk_sz bytes from the HTTP stream
        int offset = 0;
        while (offset < chunk_sz) {
            int received = httpd_req_recv(req, (char *)chunk + offset, chunk_sz - offset);
            if (received <= 0) {
                if (received == HTTPD_SOCK_ERR_TIMEOUT) continue;
                free(chunk);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv fail");
                return ESP_FAIL;
            }
            offset += received;
        }

        // Enqueue chunk — feeder task takes ownership and frees after playback.
        // Blocks up to 500ms if queue is full (feeder is draining).
        dog_audio_play_async(chunk, chunk_sz);
        remaining -= chunk_sz;
    }

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}
#endif

// ══════════════════════════════════════════════════════════════
//  Server startup
// ══════════════════════════════════════════════════════════════
void webserver_start(void) {
    if (s_server != NULL) {
        ESP_LOGW("WEB", "Already running");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.server_port      = WEB_SERVER_PORT;
    config.max_uri_handlers = 30;
    config.recv_wait_timeout  = 30;   // 30s for OTA uploads
    config.send_wait_timeout  = 10;
    config.stack_size = 8192;         // more stack for OTA handler

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE("WEB", "Failed to start HTTP server");
        return;
    }

    // Table-driven registration
    static const httpd_uri_t uris[] = {
        { "/",         HTTP_GET,  root_get_handler,    NULL },
        { "/led",      HTTP_GET,  led_handler,         NULL },
        { "/servo",    HTTP_GET,  servo_handler,       NULL },
#ifdef RF_RX_GPIO
        { "/send",     HTTP_GET,  send_rf_handler,     NULL },
#endif
        { "/time",     HTTP_GET,  time_handler,        NULL },
        { "/status",   HTTP_GET,  status_handler,      NULL },
        { "/schedule", HTTP_GET,  schedule_handler,    NULL },
        { "/s1on",     HTTP_GET,  quick_action_handler,NULL },
        { "/s1off",    HTTP_GET,  quick_action_handler,NULL },
        { "/s2on",     HTTP_GET,  quick_action_handler,NULL },
        { "/s2off",    HTTP_GET,  quick_action_handler,NULL },
#if SERVO_COUNT >= 3
        { "/s3on",     HTTP_GET,  quick_action_handler,NULL },
        { "/s3off",    HTTP_GET,  quick_action_handler,NULL },
#endif
#if SERVO_COUNT >= 4
        { "/s4on",     HTTP_GET,  quick_action_handler,NULL },
        { "/s4off",    HTTP_GET,  quick_action_handler,NULL },
#endif
        { "/l1on",     HTTP_GET,  quick_action_handler,NULL },
        { "/l1off",    HTTP_GET,  quick_action_handler,NULL },
        { "/toggle",   HTTP_GET,  quick_action_handler,NULL },
        { "/hi",       HTTP_GET,  quick_action_handler,NULL },
        { "/lay",      HTTP_GET,  quick_action_handler,NULL },
        { "/stand",    HTTP_GET,  quick_action_handler,NULL },
        { "/walk_fwd", HTTP_GET,  quick_action_handler,NULL },
        { "/walk_bwd", HTTP_GET,  quick_action_handler,NULL },
#ifdef WS2812_NUM_LEDS
        { "/audio",    HTTP_POST, audio_post_handler,  NULL },
        { "/audio",    HTTP_OPTIONS, cors_options_handler, NULL },
#endif
#ifndef DISABLE_OTA
        { "/ota",      HTTP_GET,  ota_page_handler,    NULL },
        { "/ota",      HTTP_POST, ota_upload_handler,  NULL },
#endif
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    ESP_LOGI("WEB", "HTTP server on port %d (%d endpoints)",
             config.server_port, (int)(sizeof(uris) / sizeof(uris[0])));
}
