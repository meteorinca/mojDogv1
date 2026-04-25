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
#include "dog_actions.h"

#include <string.h>
#include <stdio.h>

// rf.h is only present when board has an RF module
#ifdef RF_RX_GPIO
#include "rf.h"
#endif
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/socket.h>   // send(), close(), MSG_DONTWAIT

static httpd_handle_t s_server = NULL;

// ══════════════════════════════════════════════════════════════
//  SSE (Server-Sent Events) — push TTS text to browser
// ══════════════════════════════════════════════════════════════
#define SSE_MAX_CLIENTS 4
static int         s_sse_fds[SSE_MAX_CLIENTS];
static SemaphoreHandle_t s_sse_mutex = NULL;

static void sse_init(void) {
    s_sse_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) s_sse_fds[i] = -1;
}

// Push a line of text to all connected SSE clients.
// Format: "data: <text>\n\n"
void sse_broadcast_tts(const char *text) {
    if (!s_sse_mutex || !text) return;
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "data: %s\n\n", text);
    if (len <= 0) return;
    xSemaphoreTake(s_sse_mutex, portMAX_DELAY);
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_sse_fds[i] >= 0) {
            int sent = send(s_sse_fds[i], buf, len, MSG_DONTWAIT);
            if (sent < 0) {
                // Client disconnected — remove slot
                close(s_sse_fds[i]);
                s_sse_fds[i] = -1;
                ESP_LOGI("SSE", "Client slot %d removed", i);
            }
        }
    }
    xSemaphoreGive(s_sse_mutex);
}

static esp_err_t sse_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    // Send initial comment so the browser confirms connection
    httpd_resp_send_chunk(req, ": connected\n\n", HTTPD_RESP_USE_STRLEN);

    int fd = httpd_req_to_sockfd(req);
    xSemaphoreTake(s_sse_mutex, portMAX_DELAY);
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (s_sse_fds[i] < 0) {
            s_sse_fds[i] = fd;
            ESP_LOGI("SSE", "Client registered on slot %d (fd=%d)", i, fd);
            break;
        }
    }
    xSemaphoreGive(s_sse_mutex);

    // Return immediately — do NOT block the httpd worker thread.
    // The socket stays open because we never send the final chunk.
    // sse_broadcast_tts() pushes data directly via send() on the raw fd.
    return ESP_OK;
}

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
    else if (strcmp(action, "bark")   == 0) dog_audio_play_tone();
    else if (strcmp(action, "paulbot")== 0) dog_audio_play_paulbot();
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
    // tts:<text> — push text to SSE clients for browser-side synthesis
    else if (strncmp(action, "tts:", 4) == 0) sse_broadcast_tts(action + 4);
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

// ── /dog POST handler — same JSON protocol as ESP-Hi example /control ────────
// Body: {"action":"4"}  or  {"move":"F"}
static esp_err_t dog_handler(httpd_req_t *req) {
    char body[64] = {0};
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[got] = '\0';

    char key[16] = {0}, val[16] = {0};
    // Parse {"key":"val"}
    sscanf(body, "{\"%[^\"]\":\"%[^\"]\"}", key, val);

    bool ok = false;
    if (strcmp(key, "move") == 0 || strcmp(key, "action") == 0) {
        ok = dog_action_send(val);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, ok ? "{\"code\":200}" : "{\"error\":\"unknown\"}",
                    HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    static const char html[] =
        "<!DOCTYPE html>"
        "<html lang='en'>"
        "<head>"
        "<meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
        "<meta name='apple-mobile-web-app-capable' content='yes'>"
        "<title>MojDog Control</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent;}"
        "html,body{height:100%;overflow:hidden;position:fixed;width:100%;font-family:system-ui,sans-serif;background:#0d0d1a;color:#e0e0f0;}"
        ".tabs{display:flex;border-bottom:1px solid #1e1e3a;position:fixed;top:0;left:0;right:0;background:#0d0d1a;z-index:1000;}"
        ".tab{flex:1;padding:14px 8px;text-align:center;cursor:pointer;font-size:13px;font-weight:600;color:#666;letter-spacing:.5px;transition:color .2s,border-color .2s;border-bottom:2px solid transparent;}"
        ".tab.active{color:#7c6af7;border-bottom-color:#7c6af7;}"
        ".content{margin-top:52px;height:calc(100vh - 52px);overflow-y:auto;-webkit-overflow-scrolling:touch;}"
        "/* Control panel */"
        ".panel-ctrl{display:flex;flex-direction:column;align-items:center;height:calc(100vh - 52px);position:relative;padding:0;}"
        "/* Action buttons */"
        ".action-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:6px;padding:12px 10px 8px;width:100%;max-width:460px;}"
        ".act-btn{padding:10px 4px;font-size:12px;font-weight:600;background:linear-gradient(135deg,#1a1a35,#20203d);border:1px solid #2a2a50;border-radius:10px;color:#a0a0d0;cursor:pointer;transition:all .15s;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;}"
        ".act-btn:active,.act-btn:hover{background:linear-gradient(135deg,#7c6af7,#5b4de8);border-color:#7c6af7;color:#fff;transform:scale(.97);}"
        "/* Joystick zone */"
        ".joy-zone{flex:1;width:100%;max-width:460px;border:2px dashed #1e1e3a;border-radius:16px;margin:0 10px 10px;display:flex;flex-direction:column;justify-content:center;align-items:center;position:relative;overflow:hidden;}"
        ".joy-hint{color:#333;font-size:13px;pointer-events:none;}"
        ".err-msg{color:#f7736a;font-size:12px;margin-top:6px;text-align:center;}"
        "/* TTS panel */"
        ".panel-tts{padding:20px;}"
        ".card{background:#11112a;border:1px solid #1e1e3a;border-radius:14px;padding:18px;margin-bottom:14px;}"
        ".card h2{font-size:13px;font-weight:700;color:#7c6af7;text-transform:uppercase;letter-spacing:1px;margin-bottom:12px;}"
        ".time-big{font-size:36px;font-weight:700;font-family:monospace;color:#00e5a0;}"
        ".epoch{font-size:12px;color:#444;margin-top:4px;}"
        "input[type=text]{width:100%;padding:10px 12px;border-radius:8px;border:1px solid #2a2a50;background:#0a0a1e;color:#e0e0f0;font-size:15px;margin-bottom:10px;outline:none;}"
        "input[type=text]:focus{border-color:#7c6af7;}"
        ".btn-primary{width:100%;padding:12px;border:none;border-radius:10px;background:linear-gradient(135deg,#7c6af7,#5b4de8);color:#fff;font-size:15px;font-weight:700;cursor:pointer;transition:opacity .2s;}"
        ".btn-primary:active{opacity:.8;}"
        ".status-badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:11px;font-weight:700;background:#1e1e3a;color:#7c6af7;margin-bottom:6px;}"
        "/* Calibration */"
        ".panel-cal{padding:16px;}"
        ".vehicle{position:relative;width:180px;height:260px;background:#11112a;border-radius:18px;margin:20px auto;border:1px solid #1e1e3a;}"
        ".servo{position:absolute;width:56px;height:56px;background:#1a1a35;border:1px solid #2a2a50;border-radius:10px;display:flex;flex-direction:column;justify-content:center;align-items:center;cursor:pointer;font-size:10px;color:#888;text-align:center;transition:all .15s;}"
        ".servo.active{background:linear-gradient(135deg,#7c6af7,#5b4de8);border-color:#7c6af7;color:#fff;}"
        ".servo .label{font-size:9px;}"
        ".servo .value{font-size:13px;font-weight:700;}"
        ".servo.fl{left:-28px;top:24px;} .servo.bl{left:-28px;bottom:16px;}"
        ".servo.fr{right:-28px;top:24px;} .servo.br{right:-28px;bottom:16px;}"
        ".dog-head{position:absolute;top:-14px;left:50%;transform:translateX(-50%);width:140px;height:28px;background:#11112a;border-radius:8px;border:1px solid #1e1e3a;display:flex;align-items:center;justify-content:center;font-size:11px;color:#7c6af7;font-weight:700;}"
        ".adj{display:none;align-items:center;justify-content:center;gap:16px;margin:12px auto;}"
        ".adj-btn{width:44px;height:44px;border:none;border-radius:10px;background:#1a1a35;color:#e0e0f0;font-size:22px;cursor:pointer;transition:background .15s;}"
        ".adj-btn:active{background:#7c6af7;}"
        "#val-disp{font-size:18px;font-weight:700;min-width:50px;text-align:center;color:#7c6af7;}"
        ".cal-btn{display:block;width:100%;max-width:280px;margin:10px auto;padding:12px;border:none;border-radius:10px;background:linear-gradient(135deg,#7c6af7,#5b4de8);color:#fff;font-size:14px;font-weight:700;cursor:pointer;}"
        ".cal-intro p,.cal-intro li{font-size:13px;color:#888;margin:6px 0;line-height:1.5;}"
        ".cal-intro ol{padding-left:20px;}"
        "</style>"
        "</head><body>"
        "<div class='content' style='margin-top:0; height:100vh; padding:10px; display:flex; flex-direction:column; align-items:center;'>"
        /* ── CONTROL & TTS TAB COMBINED ── */
        "<div style='width:100%; max-width:460px;'>"
        
        "<div class='card' style='text-align:center;'>"
        "<span class='status-badge' id='conn-badge'>●&nbsp;Online</span>"
        "<div class='time-big' id='clock'>--:--:--</div>"
        "<div class='epoch'>Epoch: <span id='epoch'>-</span>&nbsp;&nbsp;Synced: <span id='synced'>no</span></div>"
        "</div>"

        "<div class='card'>"
        "<h2>Actions</h2>"
        "<div class='action-grid' id='action-grid'></div>"
        "<div class='err-msg' id='ctrl-err'></div>"
        "</div>"

        "<div class='card'>"
        "<h2>Schedule Action</h2>"
        "<div style='display:flex;gap:10px;'>"
        "<input type='number' id='sched-delay' placeholder='Delay (sec)' value='5' style='flex:1; margin-bottom:0;'>"
        "<select id='sched-action' style='flex:2; padding:10px 12px; border-radius:8px; border:1px solid #2a2a50; background:#0a0a1e; color:#e0e0f0; outline:none;'>"
        "<option value='hi'>Say Hi</option><option value='bark'>Bark</option><option value='wiggle'>Wiggle</option><option value='stand'>Stand</option><option value='lay'>Lay</option>"
        "</select>"
        "</div>"
        "<button class='btn-primary' onclick='scheduleAction()' style='margin-top:10px;'>Schedule</button>"
        "<div class='err-msg' id='sched-err'></div>"
        "</div>"

        "<div class='card' id='tts-card'>"
        "<h2>Text to Speech</h2>"
        "<input type='text' id='say' placeholder='What should MojDog say?'>"
        "<button class='btn-primary' onclick='sendTTS()'>Speak</button>"
        "<div class='err-msg' id='tts-err'></div>"
        "</div>"
        
        "</div>"
        /* ── CALIBRATION SECTION (Hidden by default, can be toggled) ── */
        "<button onclick=\"document.getElementById('panel-cal').style.display='block'\" style='margin-top:20px; background:none; border:none; color:#666; text-decoration:underline;'>Show Calibration</button>"
        "<div id='panel-cal' style='display:none; width:100%; max-width:460px; margin-top:20px;'>"
        "<div class='panel-cal'>"
        "<div id='cal-intro' class='cal-intro'>"
        "<div class='card'>"
        "<h2>Servo Calibration</h2>"
        "<p>If legs are not level after assembly, use this to fine-tune each servo's neutral position.</p>"
        "<ol><li>Press 'Enter Calibration Mode'.</li><li>Tap each leg corner to select it, then ± to adjust.</li><li>Press 'Exit Calibration Mode'.</li></ol>"
        "</div>"
        "<button class='cal-btn' id='btn-start-cal'>Enter Calibration Mode</button>"
        "</div>"
        "<div id='cal-iface' style='display:none'>"
        "<div class='vehicle'>"
        "<div class='dog-head'>MojDog</div>"
        "<div class='servo fl' data-pos='fl'><span class='label'>FL</span><span class='value' id='v-fl'>0</span></div>"
        "<div class='servo fr' data-pos='fr'><span class='label'>FR</span><span class='value' id='v-fr'>0</span></div>"
        "<div class='servo bl' data-pos='bl'><span class='label'>BL</span><span class='value' id='v-bl'>0</span></div>"
        "<div class='servo br' data-pos='br'><span class='label'>BR</span><span class='value' id='v-br'>0</span></div>"
        "</div>"
        "<div class='adj' id='adj'>"
        "<button class='adj-btn' id='btn-minus'>−</button>"
        "<code id='val-disp'>0</code>"
        "<button class='adj-btn' id='btn-plus'>+</button>"
        "</div>"
        "<button class='cal-btn' id='btn-exit-cal'>Exit Calibration Mode</button>"
        "<div class='err-msg' id='cal-err'></div>"
        "</div>"
        "</div></div>"
        "</div>" /* end .content */
        /* ── No external CDN scripts — joystick replaced with simple d-pad ── */
        "<script>"
        /* Scripts */
        "function scheduleAction(){"
        "var d=document.getElementById('sched-delay').value;"
        "var a=document.getElementById('sched-action').value;"
        "fetch('/schedule?action='+a+'&delay='+d).then(function(r){return r.json();})"
        ".then(function(j){ document.getElementById('sched-err').textContent='Scheduled for +'+d+'s'; })"
        ".catch(function(e){ document.getElementById('sched-err').textContent=e; });"
        "}"
        /* Dog action API */
        "async function dog(val){"
        "var isMove=['F','B','L','R'].indexOf(val)>=0;"
        "var body=isMove?JSON.stringify({move:val}):JSON.stringify({action:val});"
        "try{await fetch('/dog',{method:'POST',headers:{'Content-Type':'application/json'},body:body});}"
        "catch(e){document.getElementById('ctrl-err').textContent='Error: '+e;}"
        "}"
        /* Build action grid */
        "var ACTIONS={"
        "'1':'Lie','2':'Bow','3':'Lean','4':'Wiggle',"
        "'5':'Rock','6':'Sway','7':'Shake','8':'Poke',"
        "'9':'Kick','10':'Jump→','11':'←Jump','12':'Stand'};"
        "(function(){"
        "var g=document.getElementById('action-grid');"
        "for(var k in ACTIONS){"
        "var b=document.createElement('button');"
        "b.className='act-btn';b.textContent=ACTIONS[k];"
        "(function(key){b.onclick=function(){dog(key);};})(k);"
        "g.appendChild(b);"
        "}"
        "})();"
        /* Clock — sync immediately, then every 10s. Tick locally every 100ms. */
        "var timeOffset=0,isSynced=false;"
        "function syncT(){"
        "fetch('/time').then(function(r){return r.json();}).then(function(d){"
        "if(d.synced){"
        "  timeOffset = (d.epoch * 1000) - Date.now();"
        "  isSynced = true;"
        "  document.getElementById('synced').textContent='yes';"
        "}else{"
        "  fetch('/sync_time?epoch='+Math.floor(Date.now()/1000));"
        "  timeOffset = 0; isSynced = true;"
        "  document.getElementById('synced').textContent='local';"
        "}"
        "}).catch(function(){});"
        "}"
        "syncT(); setInterval(syncT, 10000);"
        "setInterval(function(){"
        "var d=new Date(Date.now()+timeOffset);"
        "var f=d.getFullYear()+'-'+('0'+(d.getMonth()+1)).slice(-2)+'-'+('0'+d.getDate()).slice(-2)+' '+"
        "('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)+':'+('0'+d.getSeconds()).slice(-2);"
        "document.getElementById('clock').textContent=f;"
        "document.getElementById('epoch').textContent=Math.floor(d.getTime()/1000);"
        "},100);"
        /* TTS — dual path: browser speaks instantly + bot speaker via /audio */
        "function sendTTS(){"
        "var t=document.getElementById('say').value.trim();"
        "if(!t)return;"
        "var err=document.getElementById('tts-err');"
        /* Fetch MP3 from Google TTS (via proxy), resample, POST to bot */
        "err.textContent='Sending to bot...';"
        "fetch('https://api.codetabs.com/v1/proxy?quest='+encodeURIComponent('https://translate.google.com/translate_tts?ie=UTF-8&q='+encodeURIComponent(t)+'&tl=en&client=tw-ob'))"
        ".then(function(r){ if(!r.ok) throw new Error('HTTP '+r.status); return r.arrayBuffer(); })"
        ".then(function(ab){"
        "var ctx=new(window.AudioContext||window.webkitAudioContext)();"
        "return ctx.decodeAudioData(ab).then(function(dec){"
        "var samples=Math.ceil(dec.duration*16000);"
        "if(samples<1)throw new Error('empty');"
        "var off=new OfflineAudioContext(1,samples,16000);"
        "var src=off.createBufferSource();"
        "src.buffer=dec;src.connect(off.destination);src.start();"
        "return off.startRendering();"
        "}).then(function(ren){"
        "var dat=ren.getChannelData(0);"
        "var pcm=new Int16Array(dat.length);"
        "for(var i=0;i<dat.length;i++){var s=Math.max(-1,Math.min(1,dat[i]));pcm[i]=s<0?s*0x8000:s*0x7FFF;}"
        "var chunkSamples=16000; var idx=0;"
        "function sendNext(){"
        "  if(idx*chunkSamples>=pcm.length){err.textContent='';return;}"
        "  var sl=pcm.slice(idx*chunkSamples, (idx+1)*chunkSamples);"
        "  idx++;"
        "  fetch('/audio',{method:'POST',body:sl.buffer}).then(sendNext).catch(function(e){err.textContent='Bot audio err: '+e;});"
        "}"
        "sendNext();"
        "});"
        "}).catch(function(e){err.textContent='Bot audio err (need internet): '+e;});"
        "}"
        "/* SSE auto-update — reconnects on drop */"
        "function initSSE(){"
        "var es=new EventSource('/events');"
        "es.onmessage=function(e){};"
        "es.onerror=function(){es.close();setTimeout(initSSE,8000);};"
        "}"
        "initSSE();"
        /* Calibration */
        "var CAL={fl:0,fr:0,bl:0,br:0},curServo=null;"
        "document.getElementById('btn-start-cal').onclick=function(){"
        "fetch('/start_calibration').then(function(r){return r.json();}).then(function(d){"
        "CAL=d;"
        "['fl','fr','bl','br'].forEach(function(k){document.getElementById('v-'+k).textContent=CAL[k];});"
        "document.getElementById('cal-intro').style.display='none';"
        "document.getElementById('cal-iface').style.display='';"
        "}).catch(function(){document.getElementById('cal-err').textContent='Could not enter calibration mode';});"
        "};"
        "document.getElementById('btn-exit-cal').onclick=function(){"
        "fetch('/exit_calibration').then(function(){document.getElementById('cal-iface').style.display='none';document.getElementById('cal-intro').style.display='';}).catch(function(){});"
        "};"
        "document.querySelectorAll('.servo').forEach(function(s){"
        "s.onclick=function(){"
        "document.querySelectorAll('.servo').forEach(function(x){x.classList.remove('active');});"
        "s.classList.add('active');curServo=s.dataset.pos;"
        "document.getElementById('val-disp').textContent=CAL[curServo];"
        "document.getElementById('adj').style.display='flex';"
        "};"
        "});"
        "function doAdj(delta){"
        "if(!curServo)return;"
        "CAL[curServo]=Math.max(-25,Math.min(25,CAL[curServo]+delta));"
        "document.getElementById('v-'+curServo).textContent=CAL[curServo];"
        "document.getElementById('val-disp').textContent=CAL[curServo];"
        "fetch('/adjust',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({servo:curServo,value:CAL[curServo]})});"
        "}"
        "document.getElementById('btn-minus').onclick=function(){doAdj(-1);};"
        "document.getElementById('btn-plus').onclick=function(){doAdj(1);};"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ══════════════════════════════════════════════════════════════
//  Calibration endpoints
//  The Calibrate tab JS does:
//    GET /start_calibration  → returns {"fl":0,"fr":0,"bl":0,"br":0}
//    POST /adjust            → {"servo":"fl","value":3}
//    GET /exit_calibration   → returns {"code":200}
// ══════════════════════════════════════════════════════════════
static struct { int fl, fr, bl, br; } s_cal = {0};
static bool s_cal_mode = false;

static void cal_apply(void) {
    servo_set_angle(1, 90 + s_cal.fl);   // FL
    servo_set_angle(2, 90 - s_cal.fr);   // FR (mirrored)
    servo_set_angle(3, 90 - s_cal.bl);   // BL (mirrored)
    servo_set_angle(4, 90 + s_cal.br);   // BR
}

static esp_err_t start_calibration_get_handler(httpd_req_t *req) {
    s_cal_mode = true;
    cal_apply();
    char resp[64];
    int len = snprintf(resp, sizeof(resp),
        "{\"fl\":%d,\"fr\":%d,\"bl\":%d,\"br\":%d}",
        s_cal.fl, s_cal.fr, s_cal.bl, s_cal.br);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
    return ESP_OK;
}

static esp_err_t exit_calibration_get_handler(httpd_req_t *req) {
    s_cal_mode = false;
    // Return servos to plain neutral
    servo_set_angle(1, 90); servo_set_angle(2, 90);
    servo_set_angle(3, 90); servo_set_angle(4, 90);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"code\":200}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t adjust_handler(httpd_req_t *req) {
    if (!s_cal_mode) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not in calibration mode");
        return ESP_FAIL;
    }
    char body[64] = {0};
    int got = httpd_req_recv(req, body, sizeof(body) - 1);
    if (got > 0) {
        char leg[8] = {0};
        int val = 0;
        sscanf(body, "{\"servo\":\"%[^\"]\",\"value\":%d}", leg, &val);
        if      (strcmp(leg, "fl") == 0) s_cal.fl = val;
        else if (strcmp(leg, "fr") == 0) s_cal.fr = val;
        else if (strcmp(leg, "bl") == 0) s_cal.bl = val;
        else if (strcmp(leg, "br") == 0) s_cal.br = val;
        cal_apply();
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"code\":200}", HTTPD_RESP_USE_STRLEN);
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
    // Avoid cJSON heap alloc/free on every poll — plain snprintf instead
    char buf[32];
    timekeep_format(buf, sizeof(buf));
    char resp[96];
    int len = snprintf(resp, sizeof(resp),
        "{\"formatted\":\"%s\",\"epoch\":%lld,\"synced\":%s}",
        buf, (long long)timekeep_now(),
        timekeep_is_synced() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// /s{N}_{angle} — set servo N to exact angle (holds, no return-to-neutral)
// Example: /s1_90  /s2_0  /s3_180
static esp_err_t servo_angle_uri_handler(httpd_req_t *req) {
    const char *uri = req->uri; // e.g. "/s1_90"
    int servo = 0, angle = 0;
    if (sscanf(uri, "/s%d_%d", &servo, &angle) == 2) {
        if (servo >= 1 && servo <= servo_count()) {
            servo_action_set(servo, angle);
            ESP_LOGI("WEB", "API: servo %d -> %d deg", servo, angle);
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t tts_api_handler(httpd_req_t *req) {
    char text[256] = {0};
    const char *uri = req->uri;

    // Check for path-style: /sendtts:...
    if (strncmp(uri, "/sendtts:", 9) == 0) {
        strncpy(text, uri + 9, sizeof(text) - 1);
    } else {
        // Query-string style: /tts?say=...
        char buf[300];
        if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
            httpd_query_key_value(buf, "say", text, sizeof(text));
        }
    }

    // URL-decode '+' as space (minimal decoder for simple phrases)
    for (char *p = text; *p; p++) if (*p == '+') *p = ' ';

    if (text[0]) {
        ESP_LOGI("WEB", "TTS API: \"%s\"", text);
        sse_broadcast_tts(text);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, text[0] ? "OK" : "Missing ?say=", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t eye_mood_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "val", param, sizeof(param)) == ESP_OK) {
            dog_set_eye_mood(atoi(param));
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t oled_text_handler(httpd_req_t *req) {
    char buf[200];
    char msg[32] = {0};
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        httpd_query_key_value(buf, "msg", msg, sizeof(msg));
    }
    for (char *p = msg; *p; p++) if (*p == '+') *p = ' '; // simple URL decode
    if (msg[0]) {
        dog_set_oled_text(msg, 3000); // show for 3 seconds
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t sync_time_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "epoch", param, sizeof(param)) == ESP_OK) {
            time_t t = strtol(param, NULL, 10);
            timekeep_set_time(t);
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ── GitHub SPEAKTHISDOG.txt polling task ─────────────────────────────────────
#define GITHUB_TTS_URL "https://raw.githubusercontent.com/meteorinca/mojDogv1/refs/heads/main/SPEAKTHISDOG.txt"

static char s_last_spoken[256] = {0}; // avoid re-speaking same text

static void github_tts_task(void *arg) {
    // Wait for time sync before starting
    for (int i = 0; i < 60 && !timekeep_is_synced(); i++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    ESP_LOGI("GH_TTS", "Starting GitHub TTS polling (every 15 min)");

    while (1) {
        // Sleep until the next 15-minute mark (:00, :15, :30, :45)
        time_t now = timekeep_now();
        struct tm ti;
        localtime_r(&now, &ti);
        int min_in_quarter = ti.tm_min % 15;
        int secs_to_next   = (14 - min_in_quarter) * 60 + (60 - ti.tm_sec);
        if (secs_to_next > 900) secs_to_next = 900;
        ESP_LOGI("GH_TTS", "Next fetch in %d s", secs_to_next);
        vTaskDelay(pdMS_TO_TICKS((uint32_t)secs_to_next * 1000));

        // Fetch the raw text
        char buf[256] = {0};
        esp_http_client_config_t cfg = {
            .url            = GITHUB_TTS_URL,
            .timeout_ms     = 8000,
            .skip_cert_common_name_check = true, // fleet devices may lack CA bundle
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) { vTaskDelay(pdMS_TO_TICKS(60000)); continue; }

        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            int content_len = esp_http_client_fetch_headers(client);
            if (content_len > 0 && content_len < (int)sizeof(buf) - 1) {
                esp_http_client_read(client, buf, content_len);
                buf[content_len] = '\0';
            } else if (content_len < 0) {
                // chunked — read up to buf size
                int r = esp_http_client_read(client, buf, sizeof(buf) - 1);
                if (r > 0) buf[r] = '\0';
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        // Trim trailing whitespace / newlines
        int blen = strlen(buf);
        while (blen > 0 && (buf[blen-1] == '\n' || buf[blen-1] == '\r' || buf[blen-1] == ' ')) {
            buf[--blen] = '\0';
        }

        if (blen > 0 && strcmp(buf, s_last_spoken) != 0) {
            ESP_LOGI("GH_TTS", "Speaking: \"%s\"", buf);
            strncpy(s_last_spoken, buf, sizeof(s_last_spoken) - 1);
            sse_broadcast_tts(buf);
        } else {
            ESP_LOGI("GH_TTS", "Text unchanged, skipping speak");
        }
    }
}

// /schedule — queue an action at an exact wall-clock time or relative delay.
//
// Params (all GET query string):
//   action=<name>   required  e.g. wiggle, hi, stand, s1_90, s2_45, tts:hello
//   at=<epoch>      optional  Unix seconds (integer). Use this for fleet sync.
//   ms=<0-999>      optional  sub-second offset in ms (used with at= for precision)
//   delay=<sec>     optional  schedule N seconds from now
//   delay_ms=<ms>   optional  schedule N milliseconds from now (combines with delay)
//
// Returns JSON: {"ok":true,"action":"...","at":<epoch>,"ms":<ms>}
//
// JupyterLab one-liner (fleet sync):
//   import requests, time
//   t = int(time.time()) + 10   # 10 s from now
//   for n in range(1, 4):
//       requests.get(f"http://dogbot{n}.local:81/schedule?action=wiggle&at={t}")
static esp_err_t schedule_handler(httpd_req_t *req) {
    char qs[200];
    char action[32] = {0};
    int  delay_sec  = 0;
    int  delay_ms   = 0;
    int  extra_ms   = 0;
    time_t at       = 0;

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char p[64];
        if (httpd_query_key_value(qs, "action",   p, sizeof(p)) == ESP_OK)
            strncpy(action, p, sizeof(action) - 1);
        if (httpd_query_key_value(qs, "at",       p, sizeof(p)) == ESP_OK)
            at = (time_t)strtol(p, NULL, 10);
        if (httpd_query_key_value(qs, "ms",       p, sizeof(p)) == ESP_OK)
            extra_ms = atoi(p);
        if (httpd_query_key_value(qs, "delay",    p, sizeof(p)) == ESP_OK)
            delay_sec = atoi(p);
        if (httpd_query_key_value(qs, "delay_ms", p, sizeof(p)) == ESP_OK)
            delay_ms  = atoi(p);
    }

    if (!action[0]) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"Missing ?action=\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // Resolve the target time
    if (at == 0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        at       = tv.tv_sec + delay_sec + (delay_ms / 1000);
        extra_ms = (tv.tv_usec / 1000) + (delay_ms % 1000);
        if (extra_ms >= 1000) { at++; extra_ms -= 1000; }
    }

    timekeep_schedule_ms(action, at, extra_ms);

    char resp[128];
    int len = snprintf(resp, sizeof(resp),
        "{\"ok\":true,\"action\":\"%s\",\"at\":%lld,\"ms\":%d}",
        action, (long long)at, extra_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, len);
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

    sse_init();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.server_port      = WEB_SERVER_PORT;
    config.max_uri_handlers = 40;     // increased for new endpoints
    config.recv_wait_timeout  = 30;
    config.send_wait_timeout  = 10;
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard; // enables /s1_* /sendtts:* patterns

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE("WEB", "Failed to start HTTP server");
        return;
    }

    // Table-driven registration
    static const httpd_uri_t uris[] = {
        { "/",          HTTP_GET,  root_get_handler,       NULL },
        // Dog animation control (POST, same protocol as ESP-Hi example)
        { "/dog",       HTTP_POST, dog_handler,            NULL },
        { "/dog",       HTTP_OPTIONS, cors_options_handler,NULL },
        // Calibration endpoints (reuse example protocol)
        { "/start_calibration", HTTP_GET,  start_calibration_get_handler, NULL },
        { "/exit_calibration",  HTTP_GET,  exit_calibration_get_handler,  NULL },
        { "/adjust",            HTTP_POST, adjust_handler,                NULL },
        { "/led",       HTTP_GET,  led_handler,            NULL },
        { "/servo",     HTTP_GET,  servo_handler,          NULL },
#ifdef RF_RX_GPIO
        { "/send",      HTTP_GET,  send_rf_handler,        NULL },
#endif
        { "/time",      HTTP_GET,  time_handler,           NULL },
        { "/status",    HTTP_GET,  status_handler,         NULL },
        { "/schedule",  HTTP_GET,  schedule_handler,       NULL },
        // SSE push channel
        { "/events",    HTTP_GET,  sse_handler,            NULL },
        { "/sync_time", HTTP_GET,  sync_time_handler,      NULL },
        // OLED APIs
        { "/eye_mood",  HTTP_GET,  eye_mood_handler,       NULL },
        { "/oled_text", HTTP_GET,  oled_text_handler,      NULL },
        // Servo direct-angle shortcuts: /s1_90  /s2_0  etc.
        { "/s1_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
        { "/s2_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
#if SERVO_COUNT >= 3
        { "/s3_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
#endif
#if SERVO_COUNT >= 4
        { "/s4_*",      HTTP_GET,  servo_angle_uri_handler,NULL },
#endif
        // Named quick-actions
        { "/s1on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s1off",     HTTP_GET,  quick_action_handler,   NULL },
        { "/s2on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s2off",     HTTP_GET,  quick_action_handler,   NULL },
#if SERVO_COUNT >= 3
        { "/s3on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s3off",     HTTP_GET,  quick_action_handler,   NULL },
#endif
#if SERVO_COUNT >= 4
        { "/s4on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/s4off",     HTTP_GET,  quick_action_handler,   NULL },
#endif
        { "/l1on",      HTTP_GET,  quick_action_handler,   NULL },
        { "/l1off",     HTTP_GET,  quick_action_handler,   NULL },
        { "/toggle",    HTTP_GET,  quick_action_handler,   NULL },
        { "/hi",        HTTP_GET,  quick_action_handler,   NULL },
        { "/bark",      HTTP_GET,  quick_action_handler,   NULL },
        { "/paulbot",   HTTP_GET,  quick_action_handler,   NULL },
        { "/lay",       HTTP_GET,  quick_action_handler,   NULL },
        { "/stand",     HTTP_GET,  quick_action_handler,   NULL },
        { "/walk_fwd",  HTTP_GET,  quick_action_handler,   NULL },
        { "/walk_bwd",  HTTP_GET,  quick_action_handler,   NULL },
#ifdef WS2812_NUM_LEDS
        { "/audio",     HTTP_POST, audio_post_handler,     NULL },
        { "/audio",     HTTP_OPTIONS, cors_options_handler,NULL },
#endif

    };
    for (int i = 0; i < (int)(sizeof(uris) / sizeof(uris[0])); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    // /sendtts:... — wildcard path, registered separately
    static const httpd_uri_t sendtts_uri = {
        .uri      = "/sendtts:*",
        .method   = HTTP_GET,
        .handler  = tts_api_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &sendtts_uri);

    // GitHub SPEAKTHISDOG.txt polling task (8KB stack — uses esp_http_client)
    xTaskCreate(github_tts_task, "gh_tts", 8192, NULL, 2, NULL);

    ESP_LOGI("WEB", "HTTP server on port %d", config.server_port);
}
