#include "timekeep.h"
#include "webserver.h"
#include "config.h"
#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static bool s_synced = false;

// ── Scheduled actions ──
typedef struct {
    time_t execute_at;
    char   action[16];
    bool   active;
} sched_entry_t;

static sched_entry_t s_schedule[MAX_SCHEDULED_ACTIONS];
static SemaphoreHandle_t s_sched_mutex;

// Callback when SNTP syncs
static void time_sync_cb(struct timeval *tv) {
    s_synced = true;
    char buf[32];
    timekeep_format(buf, sizeof(buf));
    ESP_LOGI("TIME", "NTP synced: %s", buf);
}

void timekeep_init(void) {
    // Set timezone before anything else
    setenv("TZ", TIMEZONE, 1);
    tzset();

    // SNTP init — polls once per hour by default
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_set_sync_interval(3600000);   // 1 hour between re-syncs (ms)
    esp_sntp_init();

    s_sched_mutex = xSemaphoreCreateMutex();
    memset(s_schedule, 0, sizeof(s_schedule));

    ESP_LOGI("TIME", "SNTP started — server: %s", NTP_SERVER);
}

bool timekeep_is_synced(void) {
    return s_synced;
}

time_t timekeep_now(void) {
    time_t now;
    time(&now);
    return now;
}

void timekeep_format(char *buf, size_t len) {
    time_t now = timekeep_now();
    struct tm ti;
    localtime_r(&now, &ti);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &ti);
}

void timekeep_schedule(const char *action, time_t execute_at) {
    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SCHEDULED_ACTIONS; i++) {
        if (!s_schedule[i].active) {
            s_schedule[i].execute_at = execute_at;
            strncpy(s_schedule[i].action, action, sizeof(s_schedule[i].action) - 1);
            s_schedule[i].action[sizeof(s_schedule[i].action) - 1] = '\0';
            s_schedule[i].active = true;
            ESP_LOGI("SCHED", "Scheduled '%s' at %ld", action, (long)execute_at);
            break;
        }
    }
    xSemaphoreGive(s_sched_mutex);
}

static void scheduler_task(void *pvParameters) {
    while (1) {
        if (s_synced) {
            time_t now = timekeep_now();
            xSemaphoreTake(s_sched_mutex, portMAX_DELAY);
            for (int i = 0; i < MAX_SCHEDULED_ACTIONS; i++) {
                if (s_schedule[i].active && now >= s_schedule[i].execute_at) {
                    ESP_LOGI("SCHED", "Executing '%s'", s_schedule[i].action);
                    s_schedule[i].active = false;
                    // Release mutex before executing (action may take time)
                    xSemaphoreGive(s_sched_mutex);
                    execute_named_action(s_schedule[i].action);
                    // Re-take for next iteration
                    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);
                }
            }
            xSemaphoreGive(s_sched_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));   // 1 s is fine for second-resolution scheduling
    }
}

void timekeep_start_scheduler(void) {
    xTaskCreate(scheduler_task, "sched", 3072, NULL, 3, NULL);
}
