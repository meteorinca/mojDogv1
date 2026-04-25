/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_err.h"
#include "esp_log.h"
#include "app_wifi.h"
#include "servo_dog_ctrl.h"
#include "esp_hi_web_control.h"
#include "nvs_flash.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    servo_dog_ctrl_config_t config = {
        .fl_gpio_num = 21,
        .fr_gpio_num = 19,
        .bl_gpio_num = 20,
        .br_gpio_num = 18,
    };
    servo_dog_ctrl_init(&config);
    app_wifi_init();
    esp_hi_web_control_server_init();
}
