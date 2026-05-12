#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"

#include "NeonLED.h"
#include "wifi_config.h"
#include "gsm_manager.h"
#include "time_manager.h"
#include "data_reporter.h"
#include "gas_sensor.h"
#include "ota_manager.h"       // <-- MQTT OTA

static const char *TAG = "MAIN_APP";

#define MQTT_BROKER_URI   "mqtts://7079618d65c446c28ed8db5aeeba2a82.s1.eu.hivemq.cloud:8883"   // public test broker
#define MQTT_CA_CERT      NULL   // use cert bundle

#define GSM_OTA_CHECK_INTERVAL_MS (6* 60 * 1000) // Check once a day

// --- CONFIGURATION ---
#define BUTTON_GPIO     GPIO_NUM_4
#define WIFI_LED_GPIO   GPIO_NUM_18
#define ESP_INTR_FLAG_DEFAULT 0
#define LONG_PRESS_MS   6000
#define WIFI_RETRY_INTERVAL_MS      (5 * 60 * 1000)
#define STATUS_UPLOAD_INTERVAL_MS   (15 * 60 * 1000)
#define INTERNET_CHECK_INTERVAL_MS  (10 * 60 * 1000)

/* OTA check timer is removed – updates are now push via MQTT */

static int status_packet_count = 0;

SemaphoreHandle_t button_sem = NULL;
TimerHandle_t wifi_retry_timer = NULL;
TimerHandle_t internet_check_timer = NULL;

volatile bool wifi_connected = false;
volatile bool internet_available = false;

// --- WI-FI RETRY TIMER CALLBACK ---
void wifi_retry_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "Retry Timer Expired. Attempting to connect...");
    esp_wifi_connect();
}

// --- INTERNET CHECK TIMER CALLBACK ---
void internet_check_timer_callback(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "Internet Check Timer: Starting connectivity verification...");

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_connected = wifi_is_connected();
        if (wifi_connected) {
            if (ping_google()) {
                ESP_LOGI(TAG, "Internet Check: Ping successful - Internet available");
                internet_available = true;
                if (!gas_is_emergency()) {
                    uint32_t uptime_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                    if (uptime_ms > WARM_UP_DURATION_MS) {
                        gpio_set_level(WIFI_LED_GPIO, 1);
                    }
                }
                // Ensure MQTT is running (might have been stopped previously)
                // It's safe to call multiple times if already started
                // In this example we start MQTT only once after boot; here we just check.
            } else {
                ESP_LOGW(TAG, "Internet Check: Ping failed - No internet connection");
                internet_available = false;
                gpio_set_level(WIFI_LED_GPIO, 0);
            }
        } else {
            ESP_LOGW(TAG, "Internet Check: Wi-Fi not connected");
            internet_available = false;
            gpio_set_level(WIFI_LED_GPIO, 0);
        }
    }
}

// --- BUTTON LOGIC (unchanged) ---
static void IRAM_ATTR button_isr_handler(void* arg) {
    xSemaphoreGiveFromISR(button_sem, NULL);
}

static void button_task(void* arg) {
    while(1) {
        if(xSemaphoreTake(button_sem, portMAX_DELAY) == pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(BUTTON_GPIO) == 1) continue;
            int hold_time = 0;
            bool long_press_handled = false;
            while(gpio_get_level(BUTTON_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                hold_time += 100;
                if(hold_time >= LONG_PRESS_MS && !long_press_handled) {
                    ESP_LOGI(TAG, "Long Press! Starting AP Mode.");
                    long_press_handled = true;
                    gpio_set_level(WIFI_LED_GPIO, 0);
                    if(wifi_retry_timer) xTimerStop(wifi_retry_timer, 0);
                    if(internet_check_timer) xTimerStop(internet_check_timer, 0);
                    // Stop MQTT during provisioning (optional)
                    ota_deinit_mqtt();
                    neon_set_status(NEON_AP_MODE);
                    start_ap_provisioning_mode();
                }
            }
            if (!long_press_handled) {
                ESP_LOGI(TAG, "Short Press Detected.");
                if (gas_is_emergency()) {
                    ESP_LOGI(TAG, "Action: Silencing Alarm Buzzer.");
                    gas_silence_buzzer();
                } else {
                    ESP_LOGI(TAG, "Action: Showing Status LED (Green).");
                    neon_set_status(NEON_STABLE);
                }
            }
        }
    }
}

// --- EVENT HANDLER ---
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected. Reason: %d. Waiting 5 minutes...", event->reason);
        wifi_connected = false;
        internet_available = false;
        gpio_set_level(WIFI_LED_GPIO, 0);
        if (wifi_retry_timer) xTimerStart(wifi_retry_timer, 0);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! IP:" IPSTR, IP2STR(&event->ip_info.ip));
        wifi_connected = true;
        gpio_set_level(WIFI_LED_GPIO, 1);
        if (internet_check_timer && !xTimerIsTimerActive(internet_check_timer)) {
            xTimerStart(internet_check_timer, 0);
        }
        // LED priority
        if (!gas_is_emergency()) {
            uint32_t uptime_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (uptime_ms > WARM_UP_DURATION_MS) {
                neon_set_status(NEON_STABLE);
            }
        }
        if (wifi_retry_timer) xTimerStop(wifi_retry_timer, 0);
        // Start MQTT OTA now that we have an IP address (and hopefully internet)
        // Provide your broker URI and optional CA certificate.
        // Example: "mqtts://a3xxxxxxxxxx-ats.iot.us-east-1.amazonaws.com:8883"
        // #ifndef OTA_BROKER_URI
        // #define OTA_BROKER_URI "mqtts://192.168.0.112:8883"
        // #endif
        // Use ESP bundle (no custom CA)
        ota_init_mqtt(MQTT_BROKER_URI, MQTT_CA_CERT);
    }
}

// --- LEGACY BOOT SEQUENCE (unchanged) ---
static void run_legacy_boot_sequence(void) {
    // ... same as before ...
    ESP_LOGI(TAG, ">>> LEGACY BOOT SEQUENCE STARTED <<<");
    gpio_config_t buzzer_conf = {
        .pin_bit_mask = (1ULL << BUZZER_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&buzzer_conf);
    ESP_LOGI(TAG, "Buzzer ON");  gpio_set_level(BUZZER_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "LED: Red");   neon_set_status(NEON_FAIL);     vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "LED: Green"); neon_set_status(NEON_STABLE);   vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "LED: Blue");  neon_set_status(NEON_CONNECTING); vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Buzzer OFF"); gpio_set_level(BUZZER_GPIO, 0);
    ESP_LOGI(TAG, ">>> LEGACY BOOT SEQUENCE COMPLETE <<<");
}

// --- GSM TIMESTAMP RE-CHECK (unchanged) ---
static void recheck_gsm_timestamp(void) {
    // ... same as before ...
    ESP_LOGI(TAG, "=== 10 Status Packets Sent - Re-checking GSM Timestamp ===");
    char gsm_time_str[32] = {0};
    if (gsm_get_time_str(gsm_time_str, sizeof(gsm_time_str))) {
        time_t gsm_time = atol(gsm_time_str);
        time_t current_rtc = time(NULL);
        ESP_LOGI(TAG, "GSM Time: %s (%lld)", gsm_time_str, gsm_time);
        ESP_LOGI(TAG, "RTC Time: %lld", current_rtc);
        time_t time_diff = (gsm_time > current_rtc) ? (gsm_time - current_rtc) : (current_rtc - gsm_time);
        ESP_LOGI(TAG, "Time gap: %lld seconds (%lld minutes)", time_diff, time_diff / 60);
        if (time_diff >= 900) {
            ESP_LOGW(TAG, "Time difference is %lld seconds (>= 15 mins). Updating RTC with GSM time.", time_diff);
            struct timeval tv = { .tv_sec = gsm_time, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "RTC updated. Sending immediate status packet...");
            data_reporter_send_status();
        } else {
            ESP_LOGI(TAG, "Time difference is %lld seconds (< 15 mins). No update needed.", time_diff);
        }
    } else {
        ESP_LOGE(TAG, "Failed to get GSM time for re-check");
    }
}

// --- MAIN APPLICATION ENTRY ---
void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware version: %s", app_desc->version);

    // 1. NVS Init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Hardware Init
    gpio_set_direction(WIFI_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(WIFI_LED_GPIO, 0);
    neon_init();

    // 3. RUN LEGACY BOOT SEQUENCE
    run_legacy_boot_sequence();

    // 4. START GAS SENSOR
    gas_sensor_init();

    button_sem = xSemaphoreCreateBinary();
    gpio_config_t btn_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .pin_bit_mask = (1ULL << BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = 1
    };
    gpio_config(&btn_conf);
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);

    // 5. Network Init
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_retry_timer = xTimerCreate("WiFiRetry", pdMS_TO_TICKS(WIFI_RETRY_INTERVAL_MS), pdFALSE, (void*)0, wifi_retry_timer_callback);
    internet_check_timer = xTimerCreate("InternetCheck", pdMS_TO_TICKS(INTERNET_CHECK_INTERVAL_MS), pdTRUE, (void*)0, internet_check_timer_callback);

    xTaskCreate(button_task, "button_task", 5120, NULL, 10, NULL);

    // 6. Start GSM
    gsm_init();
    gsm_power_on();
    gsm_start_monitoring();

    // 7. Start Wi-Fi
    wifi_init_and_check();

    // 8. Time Manager Init
    time_manager_init();

    // --- STARTUP SEQUENCE ---
    ESP_LOGI(TAG, "=== STARTUP: Waiting 20s for networks to stabilize ===");
    vTaskDelay(pdMS_TO_TICKS(20000));

    // 9. Sync Time
    ESP_LOGI(TAG, "=== STARTUP: Syncing Time ===");
    bool time_valid = false;
    if (wifi_connected) {
        ESP_LOGI(TAG, "Wi-Fi is Connected. Attempting SNTP Sync...");
        if (time_sync()) {
            time_valid = true;
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi is Disconnected. Skipping SNTP.");
    }
    if (!time_valid) {
        ESP_LOGW(TAG, "Time Invalid. Fetching from GSM Network...");
        char gsm_ts[32];
        if (gsm_get_time_str(gsm_ts, sizeof(gsm_ts))) {
            time_valid = true;
        }
    }

    // 10. Send "Device Ready" Packet
    if (time_valid) {
        ESP_LOGI(TAG, "Time Synced Successfully");
        ESP_LOGI(TAG, "=== STARTUP: Sending Device Ready Packet ===");
        data_reporter_send_ready();
    } else {
        ESP_LOGE(TAG, "Time Sync Failed. Skipping Start Packet.");
    }

    // 11. Start periodic timers
    if (internet_check_timer) {
        xTimerStart(internet_check_timer, 0);
    }

    /* MQTT OTA will be started in the IP_EVENT_STA_GOT_IP handler */

    // --- MAIN LOOP ---
    uint32_t last_status_time = xTaskGetTickCount();
    while(1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint32_t now = xTaskGetTickCount();

        // STATUS UPLOAD (Every 15 Minutes)
        // (Make sure gsm_is_ota_busy() is still defined in gsm_manager; it doesn't interfere with MQTT)
        if (!gsm_is_ota_busy() && (now - last_status_time) >= pdMS_TO_TICKS(STATUS_UPLOAD_INTERVAL_MS)) {
            ESP_LOGI(TAG, "15-Minute Timer: Sending Status Packet...");
            if (data_reporter_send_status()) {
                ESP_LOGI(TAG, "Status Packet SENT");
                status_packet_count++;
                if (status_packet_count >= 10) {
                    status_packet_count = 0;
                    recheck_gsm_timestamp();
                }
            } else {
                ESP_LOGE(TAG, "Status Packet FAILED");
            }
            last_status_time = now;
        }
    }
}
