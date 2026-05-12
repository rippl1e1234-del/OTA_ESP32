#include "gas_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "NeonLED.h"
#include "data_reporter.h"
#include "esp_wifi.h" 

static const char *TAG = "GAS_SENS";

// --- ACTUATOR CONFIG ---
#ifdef RLY_PIN_ALT
#define RLY_PIN     GPIO_NUM_2   
#else
#define RLY_PIN     GPIO_NUM_15  
#endif

#define MOT1_PIN    GPIO_NUM_12   //MOT FOR
#define MOT2_PIN    GPIO_NUM_13   // MOT REV

#define VALVE_CLOSE_TIME_MS     12000   
#define RECOVERY_STABLE_TIME_MS 30000   
#define EMERGENCY_UPLINK_MS     30000   

static adc_oneshot_unit_handle_t adc_handle;
static int gas_threshold = GAS_THRESHOLD_DEFAULT;
static volatile bool emergency_state = false;
static bool valve_is_closed = false;
static uint64_t last_uplink_time = 0;
static uint64_t recovery_start_time = 0; 

#define NVS_KEY_PREHEAT "preheated"
#define ADC_UNIT            ADC_UNIT_1
#define ADC_ATTEN           ADC_ATTEN_DB_12 
#define POLL_INTERVAL_MS    100             

// --- HELPER TO CHECK AP MODE ---
// Prevents gas sensor from overwriting Magenta LED during Wi-Fi Config
static bool is_ap_mode_active(void) {
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        if (mode == WIFI_MODE_APSTA || mode == WIFI_MODE_AP) {
            return true;
        }
    }
    return false;
}

// --- ACTUATOR CONTROL ---
static void actuators_init() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RLY_PIN) | (1ULL << MOT1_PIN) | (1ULL << MOT2_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(RLY_PIN, 0);
    gpio_set_level(MOT1_PIN, 0);
    gpio_set_level(MOT2_PIN, 0);
}

static void trigger_alarm_hardware() {
    ESP_LOGE(TAG, "!!! TRIGGERING ALARM HARDWARE !!!");
    neon_set_status(NEON_FAIL); 
    gpio_set_level(BUZZER_GPIO, 1);
    ESP_LOGI(TAG, "Buzzer : ON");
    gpio_set_level(RLY_PIN, 1);
    ESP_LOGI(TAG, "RLY : ON");
    gpio_set_level(MOT1_PIN, 1);
    ESP_LOGI(TAG, "MOT_FW : ON");
    gpio_set_level(MOT2_PIN, 0);
    ESP_LOGI(TAG, "MOT_RW : OFF");
    if (!valve_is_closed) {
        vTaskDelay(pdMS_TO_TICKS(VALVE_CLOSE_TIME_MS)); 
        valve_is_closed = true;
    }
}

static void reset_alarm_hardware() {
    ESP_LOGI(TAG, "SAFE: Resetting Hardware.");
    neon_set_status(NEON_STABLE); 
    gpio_set_level(BUZZER_GPIO, 0);
    ESP_LOGI(TAG, "Buzzer : OFF");
    gpio_set_level(RLY_PIN, 1);
    ESP_LOGI(TAG, "RLY : ON");
    gpio_set_level(MOT1_PIN, 0);
    ESP_LOGI(TAG, "MOT_FW : OFF");
    gpio_set_level(MOT2_PIN, 1); 
    ESP_LOGI(TAG, "MOT_RW : ON");
    valve_is_closed = false;
}

void gas_silence_buzzer(void) {
    if (emergency_state) {
        gpio_set_level(BUZZER_GPIO, 0);
    }
}

static bool is_preheat_done(void) {
    nvs_handle_t my_handle;
    uint8_t done = 0;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_u8(my_handle, NVS_KEY_PREHEAT, &done);
        nvs_close(my_handle);
    }
    return (done == 1);
}

static void set_preheat_done(void) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_u8(my_handle, NVS_KEY_PREHEAT, 1);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

// --- MAIN LOGIC ---
static void gas_monitor_task(void *pvParameters) {
    // Check if this is a first-time boot
    bool needs_first_boot_mark = !is_preheat_done();

    if (needs_first_boot_mark) {
        ESP_LOGW(TAG, "First Boot detected. Running mandatory 3-min initialization.");
    }

    // --- CONSOLIDATED 3-MINUTE WARM-UP ---
    // This phase handles both "Pre-heat" and "Warm-up" in a single 3-minute window
    ESP_LOGI(TAG, "Starting Warm-Up Phase (%d min)...", WARM_UP_DURATION_MS / 60000);
    
    // Only set the LED to Blue if the User isn't currently in AP Mode
    if (!is_ap_mode_active()) {
        neon_set_status(NEON_CONNECTING); 
    }
    
    // Single Wait Period (3 Minutes)
    vTaskDelay(pdMS_TO_TICKS(WARM_UP_DURATION_MS));

    // Mark NVS flag as done if it was the first boot
    if (needs_first_boot_mark) {
        set_preheat_done();
        ESP_LOGI(TAG, "Pre-heat flag saved to NVS.");
    }

    // --- MONITORING START ---
    ESP_LOGI(TAG, ">>> Sensor Active. Monitoring Started. <<<");
    if (!emergency_state && !is_ap_mode_active()) {
        neon_set_status(NEON_STABLE); // Turn Green
    }

    int adc_raw;
    while (1) {
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, GAS_ADC_CHANNEL, &adc_raw));
        uint64_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        
        if (adc_raw > gas_threshold) {
            vTaskDelay(pdMS_TO_TICKS(50)); 
            adc_oneshot_read(adc_handle, GAS_ADC_CHANNEL, &adc_raw);

            if (adc_raw > gas_threshold) {
                recovery_start_time = 0; 
                if (!emergency_state) {
                    emergency_state = true;
                    trigger_alarm_hardware();
                }
                if ((now - last_uplink_time) > EMERGENCY_UPLINK_MS) {
                    data_reporter_send_emergency(adc_raw);
                    last_uplink_time = now;
                }
            }
        }
        else {
            if (emergency_state) {
                if (recovery_start_time == 0) {
                    recovery_start_time = now;
                    //gpio_set_level(BUZZER_GPIO, 0); 
                }
                if ((now - recovery_start_time) >= RECOVERY_STABLE_TIME_MS) {
                    emergency_state = false;
                    recovery_start_time = 0;
                    reset_alarm_hardware();
                    data_reporter_send_status(); 
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void gas_sensor_init(void) {
    actuators_init();
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(BUZZER_GPIO, 0);

    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, GAS_ADC_CHANNEL, &config));

    xTaskCreate(gas_monitor_task, "gas_mon", 4096, NULL, 10, NULL);
}

bool gas_is_emergency(void) {
    return emergency_state;
}
