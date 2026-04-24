#include "dog_peripherals.h"
#include "config.h"

#ifdef WS2812_NUM_LEDS

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/queue.h"
#include <string.h>
#include <math.h>

static const char *TAG = "DOG_PERIPH";

static volatile bool force_blink = false;
static volatile int eye_mood = 0; // 0=angry, 1=neutral, 2=sad
static QueueHandle_t button_evt_queue = NULL;

// --- OLED SPI Display ---
static esp_lcd_panel_handle_t panel_handle = NULL;

static void dog_eyes_task(void *arg) {
    uint16_t *buffer = malloc(160 * 80 * sizeof(uint16_t));
    if (!buffer) {
        ESP_LOGE(TAG, "No mem for eyes");
        vTaskDelete(NULL);
    }
    
    int blink_timer = 0;
    int next_blink = 50 + (rand() % 100);
    bool blinking = false;
    
    while(1) {
        blink_timer++;
        if (!blinking && blink_timer > next_blink) {
            blinking = true;
            blink_timer = 0;
        }

        if (force_blink) {
            blinking = true;
            blink_timer = 0;
            force_blink = false;
        }
        
        int cx = 80;
        int cy = 40;
        int max_rx = 60;
        int max_ry = 35;
        
        if (blinking) {
            max_ry = 4; // close the eye completely
            if (blink_timer > 3) {
                blinking = false;
                blink_timer = 0;
                next_blink = 40 + (rand() % 100);
            }
        }
        
        static int pupil_dx = 0;
        static int pupil_dy = 0;
        static int pupil_target_dx = 0;
        static int pupil_target_dy = 0;
        
        if (rand() % 20 == 0) {
            pupil_target_dx = (rand() % 20) - 10;
            pupil_target_dy = (rand() % 10) - 5;
        }
        
        // Smoothly move pupil
        if (pupil_dx < pupil_target_dx) pupil_dx++;
        if (pupil_dx > pupil_target_dx) pupil_dx--;
        if (pupil_dy < pupil_target_dy) pupil_dy++;
        if (pupil_dy > pupil_target_dy) pupil_dy--;

        for(int y = 0; y < 80; y++) {
            for(int x = 0; x < 160; x++) {
                uint16_t color = 0x0000;
                int dx = x - cx;
                int dy = y - cy;
                
                // Ellipse for a single large eye
                if ((dx * dx * max_ry * max_ry) + (dy * dy * max_rx * max_rx) <= (max_rx * max_rx * max_ry * max_ry)) {
                    int eye_lid_y = -1000;
                    
                    if (eye_mood == 0) { // Angry (slants down towards center)
                        eye_lid_y = cy - 10 - abs(dx) * 2 / 5;
                    } else if (eye_mood == 1) { // Neutral (flat eyelid)
                        eye_lid_y = cy - 15;
                    } else if (eye_mood == 2) { // Sad (slants up towards center)
                        eye_lid_y = cy - 25 + abs(dx) * 2 / 5;
                    }
                    
                    if (y > eye_lid_y || blinking) {
                        color = 0xFFFF; // Sclera (White)
                        
                        // Iris
                        int idx = dx - pupil_dx;
                        int idy = dy - pupil_dy;
                        int iris_r = 22;
                        
                        if (idx*idx + idy*idy <= iris_r*iris_r) {
                            color = 0x7E59; // Light greenish blue
                            
                            // Pupil
                            int pupil_r = 8;
                            if (idx*idx + idy*idy <= pupil_r*pupil_r) {
                                color = 0x0000; // Black
                            } else {
                                // Catchlight (reflection)
                                int hx = idx + 7;
                                int hy = idy + 7;
                                if (hx*hx + hy*hy <= 12) {
                                    color = 0xFFFF; // White
                                }
                            }
                        }
                    }
                }
                buffer[y*160 + x] = color;
            }
        }
        
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, 160, 80, buffer);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void init_display(void) {
    ESP_LOGI(TAG, "Initializing SPI for OLED");
    spi_bus_config_t buscfg = {
        .sclk_io_num = DISP_CLK_GPIO,
        .mosi_io_num = DISP_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 160 * 80 * 2 + 8
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = DISP_DC_GPIO,
        .cs_gpio_num = -1,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    
    // Gap for 160x80 ST7789 screens to fix the bottom 15% garbage
    esp_lcd_panel_set_gap(panel_handle, 0, 24);

    // Swap x and y
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, false, true);

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    xTaskCreate(dog_eyes_task, "dog_eyes", 4096, NULL, 5, NULL);
}

// --- Audio PDM & Amp ---
static i2s_chan_handle_t tx_chan = NULL;
static RingbufHandle_t audio_rb = NULL;

static void dog_audio_task(void *arg) {
    size_t w_bytes = 0;
    int16_t silence[512] = {0}; // 1024 bytes of silence
    
    // We need to handle potential byte-alignment issues from the ring buffer.
    uint8_t leftover_byte = 0;
    bool has_leftover = false;
    bool stream_active = true;

    while(1) {
        size_t item_size = 0;
        // Wait 150ms for next chunk. If we don't get one, stream is over.
        uint8_t *item = (uint8_t *)xRingbufferReceive(audio_rb, &item_size, stream_active ? pdMS_TO_TICKS(150) : portMAX_DELAY);
        
        if (item) {
            stream_active = true;
            size_t offset = 0;
            while (offset < item_size) {
                size_t chunk_bytes = item_size - offset;
                
                if (has_leftover) {
                    uint8_t sample_bytes[2] = { leftover_byte, item[offset] };
                    i2s_channel_write(tx_chan, sample_bytes, 2, &w_bytes, portMAX_DELAY);
                    has_leftover = false;
                    offset++;
                    continue;
                }
                
                size_t even_chunk = chunk_bytes & ~1;
                if (even_chunk > 0) {
                    i2s_channel_write(tx_chan, item + offset, even_chunk, &w_bytes, portMAX_DELAY);
                    offset += even_chunk;
                } else {
                    leftover_byte = item[offset];
                    has_leftover = true;
                    offset++;
                }
            }
            vRingbufferReturnItem(audio_rb, (void *)item);
        } else {
            // Stream timed out. Flush the DMA buffers with silence to stop any repeating/looping audio.
            if (stream_active) {
                for (int i = 0; i < 4; i++) {
                    i2s_channel_write(tx_chan, silence, sizeof(silence), &w_bytes, portMAX_DELAY);
                }
                stream_active = false;
                has_leftover = false;
                ESP_LOGI(TAG, "Audio stream finished, DMA flushed.");
            }
        }
    }
}

void dog_audio_play_chunk(const uint8_t *data, size_t size) {
    if (!audio_rb) return;
    size_t sent = 0;
    while (sent < size) {
        size_t to_send = size - sent;
        if (to_send > 1024) to_send = 1024;
        if (xRingbufferSend(audio_rb, data + sent, to_send, pdMS_TO_TICKS(1000)) == pdTRUE) {
            sent += to_send;
        } else {
            ESP_LOGW(TAG, "Audio RB full, retrying");
        }
    }
}

void dog_audio_play_tone(void) {
    if (!audio_rb) return;
    ESP_LOGI(TAG, "Playing 440Hz test tone...");
    int sample_rate = 16000;
    int duration_sec = 1;
    int samples = sample_rate * duration_sec;
    uint8_t *tone_buf = malloc(1024);
    if (!tone_buf) return;
    
    for (int i = 0; i < samples; i++) {
        // 440 Hz sine wave, max amplitude
        int16_t val = (int16_t)(sin(i * 440.0 * 2.0 * M_PI / sample_rate) * 32000.0);
        int buf_idx = (i % 512) * 2;
        tone_buf[buf_idx] = val & 0xFF;           // Low byte
        tone_buf[buf_idx + 1] = (val >> 8) & 0xFF; // High byte
        
        if (buf_idx == 1022 || i == samples - 1) {
            dog_audio_play_chunk(tone_buf, buf_idx + 2);
        }
    }
    free(tone_buf);
    ESP_LOGI(TAG, "Test tone finished");
}


static void init_audio(void) {
    ESP_LOGI(TAG, "Initializing PDM Audio");
    gpio_config_t amp_conf = {
        .pin_bit_mask = (1ULL << AUDIO_AMP_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp_conf);
    gpio_set_level(AUDIO_AMP_GPIO, 1);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, NULL));

    i2s_pdm_tx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = AUDIO_CLK_GPIO,
            .dout = AUDIO_DATA_GPIO,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_tx_mode(tx_chan, &pdm_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));

    // Create ring buffer for 8KB of audio chunks
    audio_rb = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);

    xTaskCreate(dog_audio_task, "dog_audio", 4096, NULL, 5, NULL);
}

// --- Microphone ADC ---
#define MIC_ADC_CHAN ADC_CHANNEL_2 // GPIO 2

static void dog_mic_task(void *arg) {
    adc_oneshot_unit_handle_t adc1_handle = (adc_oneshot_unit_handle_t)arg;
    int adc_raw;
    while(1) {
        adc_oneshot_read(adc1_handle, MIC_ADC_CHAN, &adc_raw);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void init_mic(void) {
    ESP_LOGI(TAG, "Initializing Mic on ADC1 CH2 (GPIO 2)");
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_11,
    };
    esp_err_t err = adc_oneshot_config_channel(adc1_handle, MIC_ADC_CHAN, &config);
    if(err != ESP_OK) { // fallback to 12 if 11 is undefined/fails
        config.atten = ADC_ATTEN_DB_12;
        adc_oneshot_config_channel(adc1_handle, MIC_ADC_CHAN, &config);
    }

    xTaskCreate(dog_mic_task, "dog_mic", 2048, adc1_handle, 4, NULL);
}

// --- Buttons ---
static void IRAM_ATTR button_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    if (button_evt_queue) {
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xQueueSendFromISR(button_evt_queue, &gpio_num, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void button_task(void* arg) {
    uint32_t io_num;
    while(1) {
        if(xQueueReceive(button_evt_queue, &io_num, portMAX_DELAY)) {
            vTaskDelay(pdMS_TO_TICKS(50)); // debounce
            if(gpio_get_level(io_num) == 0) {
                ESP_LOGI(TAG, "Button %lu pressed", io_num);
                if (io_num == BTN_BOOT_GPIO) {
                    eye_mood = (eye_mood + 1) % 3;
                    dog_audio_play_tone(); // audio feedback
                } else {
                    force_blink = true;
                }
            }
        }
    }
}

static void init_buttons(void) {
    ESP_LOGI(TAG, "Initializing Buttons");
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_MOVE_WAKE_GPIO) | (1ULL << BTN_AUDIO_WAKE_GPIO) | (1ULL << BTN_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_conf);
    
    button_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(button_task, "button_task", 2048, NULL, 5, NULL);

    esp_err_t err = gpio_install_isr_service(0);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "ISR install err: %d", err);
    }
    
    gpio_isr_handler_add(BTN_MOVE_WAKE_GPIO, button_isr_handler, (void*) BTN_MOVE_WAKE_GPIO);
    gpio_isr_handler_add(BTN_AUDIO_WAKE_GPIO, button_isr_handler, (void*) BTN_AUDIO_WAKE_GPIO);
    gpio_isr_handler_add(BTN_BOOT_GPIO, button_isr_handler, (void*) BTN_BOOT_GPIO);
}

void dog_peripherals_init(void) {
    ESP_LOGI(TAG, "Initializing Dog Peripherals");
    init_buttons();
    init_mic();
    init_audio();
    init_display();
}

#endif
