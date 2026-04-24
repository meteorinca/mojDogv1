#ifndef OTA_H
#define OTA_H

#include "esp_http_server.h"

// Returns the OTA upload page (GET /ota)
esp_err_t ota_page_handler(httpd_req_t *req);

// Receives firmware binary (POST /ota)
esp_err_t ota_upload_handler(httpd_req_t *req);

#endif
