#include "ota.h"
#include "config.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

esp_err_t ota_page_handler(httpd_req_t *req) {
    const char html[] =
        "<!DOCTYPE html><html><head><title>OTA Update</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:600px;margin:40px auto;padding:0 20px;}"
        "h1{color:#333;} .bar{width:100%;background:#eee;border-radius:8px;overflow:hidden;margin:10px 0;}"
        ".fill{height:28px;background:linear-gradient(90deg,#4CAF50,#45a049);width:0%;transition:width .3s;"
        "text-align:center;color:#fff;line-height:28px;font-size:14px;}"
        "#status{margin:10px 0;font-weight:bold;}"
        "button{font-size:18px;padding:10px 30px;cursor:pointer;background:#2196F3;color:#fff;"
        "border:none;border-radius:6px;} button:disabled{background:#999;}"
        "</style></head><body>"
        "<h1>OTA Firmware Update</h1>"
        "<p style='color:#d32f2f;font-size:14px;'><b>Note:</b> Upload the application binary (e.g., <code>wifisimple.bin</code>). Do <b>NOT</b> upload the merged-binary.</p>"
        "<p>Current firmware: <strong>" FW_VERSION "</strong></p>"
        "<input type='file' id='fw' accept='.bin'><br><br>"
        "<button onclick='doOTA()' id='btn'>Upload & Flash</button>"
        "<div class='bar'><div class='fill' id='prog'>0%</div></div>"
        "<div id='status'></div>"
        "<script>"
        "function doOTA(){"
        "var f=document.getElementById('fw').files[0];"
        "if(!f){alert('Select a .bin file first');return;}"
        "var btn=document.getElementById('btn');btn.disabled=true;"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('POST','/ota',true);"
        "xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "xhr.upload.onprogress=function(e){"
        "  if(e.lengthComputable){"
        "    var p=Math.round(e.loaded/e.total*100);"
        "    document.getElementById('prog').style.width=p+'%';"
        "    document.getElementById('prog').textContent=p+'%';"
        "  }"
        "};"
        "xhr.onload=function(){"
        "  document.getElementById('status').textContent="
        "    xhr.status==200?'Success! Rebooting...':'Error: '+xhr.responseText;"
        "  if(xhr.status==200) setTimeout(function(){location.reload()},5000);"
        "};"
        "xhr.onerror=function(){document.getElementById('status').textContent='Upload failed';};"
        "xhr.send(f);"
        "}"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t ota_upload_handler(httpd_req_t *req) {
    ESP_LOGI("OTA", "Starting OTA, size=%d bytes", req->content_len);

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        ESP_LOGE("OTA", "No OTA partition found — check partition table");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition. Flash with OTA partition table first.");
        return ESP_FAIL;
    }

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE("OTA", "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    int total = remaining;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf,
                           remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE("OTA", "Receive error");
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive error");
            return ESP_FAIL;
        }

        err = esp_ota_write(handle, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE("OTA", "Write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }

        remaining -= recv_len;
        if ((total - remaining) % (64 * 1024) < 1024) {
            ESP_LOGI("OTA", "Progress: %d / %d bytes", total - remaining, total);
        }
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE("OTA", "Validation failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE("OTA", "Set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return ESP_FAIL;
    }

    ESP_LOGI("OTA", "OTA complete! Rebooting...");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);

    // Give time for the HTTP response to send
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;  // never reached
}
