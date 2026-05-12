#include "NeonLED.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

#define TAG "NEON_LED"
#define LED_STRIP_GPIO_PIN 5
#define LED_STRIP_NUM_LEDS 1
#define BRIGHTNESS 20 

static led_strip_handle_t led_strip;

extern "C" void neon_init(void) {
    ESP_LOGI(TAG, "Initializing NeoPixel on GPIO %d", LED_STRIP_GPIO_PIN);

    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO_PIN,
        .max_leds = LED_STRIP_NUM_LEDS,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = false } 
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, 
        .mem_block_symbols = 0,
        .flags = { .with_dma = false }
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

extern "C" void neon_set_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (!led_strip) return;
    
    // Scale brightness
    uint8_t r_s = (r * BRIGHTNESS) / 255;
    uint8_t g_s = (g * BRIGHTNESS) / 255;
    uint8_t b_s = (b * BRIGHTNESS) / 255;
    
    led_strip_set_pixel(led_strip, 0, r_s, g_s, b_s);
    led_strip_refresh(led_strip);
}

extern "C" void neon_set_status(neon_status_t status) {
    if (!led_strip) return;

    switch (status) {
        case NEON_AP_MODE:
            ESP_LOGI(TAG, "LED: Magenta (AP Mode)");
            neon_set_rgb(255, 0, 255); 
            break;
            
        case NEON_CONNECTING:
            ESP_LOGI(TAG, "LED: Blue (Connecting)");
            neon_set_rgb(0, 0, 255);   
            break;
            
        case NEON_STABLE: 
            ESP_LOGI(TAG, "LED: Green (Stable) - stays on continuously");
            neon_set_rgb(0, 255, 0);   
            break;
            
        case NEON_FAIL:
            ESP_LOGI(TAG, "LED: Red (Fail/Alarm)");
            neon_set_rgb(255, 0, 0);   
            break;

        case NEON_COOLDOWN:
            ESP_LOGI(TAG, "LED: Yellow (Cooldown)");
            neon_set_rgb(255, 255, 0);
            break;

        case NEON_BOOT:
            neon_set_rgb(255, 255, 255);
            break;

        case NEON_OFF:
        default:
            led_strip_clear(led_strip);
            led_strip_refresh(led_strip);
            break;
    }
}
