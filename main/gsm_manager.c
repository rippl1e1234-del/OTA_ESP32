#include "gsm_manager.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "GSM_MGR";
static bool gsm_registered = false;
static SemaphoreHandle_t gsm_mutex = NULL;
static volatile bool gsm_abort_requested = false;

// ---------- OTA busy flag (prevents monitor task interference) ----------
static volatile bool gsm_ota_busy = false;

// --- Fallback APNs (used only if automatic detection fails) ---
static const char *fallback_apns[] = {
    "internet",          // most operators worldwide
    "web",               // many IoT SIMs
    "wwww",              // some private APNs
    "m2misafe",          // your previous custom APN (keep as safety net)
    NULL
};

// ---------- LED helper ----------
static void gsm_set_led(bool on) {
    gpio_set_level(GSM_LED_PIN, on ? 1 : 0);
}

// ---------- Log sanitizer ----------
static void log_sanitized(const char* prefix, uint8_t* data, int len) {
    char buf[512];
    int idx = 0;
    for (int i = 0; i < len && idx < 510; i++) {
        if (data[i] == '\r' || data[i] == '\n') {
            if (idx > 0 && buf[idx-1] != ' ') buf[idx++] = ' ';
        } else if (isprint(data[i])) {
            buf[idx++] = data[i];
        } else {
            buf[idx++] = '.';
        }
    }
    buf[idx] = '\0';
    if (idx > 0) ESP_LOGI(TAG, "%s %s", prefix, buf);
}

// ---------- Thread-safe AT command sender ----------
static bool send_at_cmd(const char *cmd, const char *expected_response, uint32_t timeout_ms) {
    if (gsm_mutex == NULL) return false;
    xSemaphoreTake(gsm_mutex, portMAX_DELAY);

    if (cmd != NULL) {
        uart_flush_input(GSM_UART_NUM);
        char clean_cmd[64];
        strncpy(clean_cmd, cmd, sizeof(clean_cmd)-1);
        clean_cmd[sizeof(clean_cmd)-1] = 0;
        char *p = strchr(clean_cmd, '\r'); if(p) *p=0;
        ESP_LOGI(TAG, "TX >> %s", clean_cmd);
        uart_write_bytes(GSM_UART_NUM, cmd, strlen(cmd));
    }

    uint8_t data[512];
    int total_len = 0;
    TickType_t start = xTaskGetTickCount();
    bool success = false;

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(timeout_ms)) {
        size_t available = 0;
        uart_get_buffered_data_len(GSM_UART_NUM, &available);
        if (available > 0) {
            int len = uart_read_bytes(GSM_UART_NUM, data + total_len,
                                     sizeof(data) - 1 - total_len,
                                     20 / portTICK_PERIOD_MS);
            if (len > 0) {
                total_len += len;
                data[total_len] = '\0';

                if (strstr((char*)data, "\n") || strstr((char*)data, "\r")) {
                    log_sanitized("RX <<", data, total_len);
                }

                if (strstr((char*)data, expected_response) != NULL) {
                    success = true;
                    break;
                }
                if (strstr((char*)data, "ERROR") != NULL) {
                    ESP_LOGW(TAG, "Command Failed (Received ERROR)");
                    success = false;
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (!success && (xTaskGetTickCount() - start) >= pdMS_TO_TICKS(timeout_ms)) {
        ESP_LOGW(TAG, "Command Timeout (No '%s' received)", expected_response);
    }

    xSemaphoreGive(gsm_mutex);
    return success;
}

// ---------- DNS Setup ----------
static void gsm_setup_dns(void) {
    if (send_at_cmd("AT+CDNSCFG=\"8.8.8.8\",\"8.8.4.4\"\r\n", "OK", 2000)) {
        ESP_LOGI(TAG, "DNS set via CDNSCFG (SIMCom)");
        return;
    }
    if (send_at_cmd("AT+QIDNSCFG=1,\"8.8.8.8\",\"8.8.4.4\"\r\n", "OK", 2000)) {
        ESP_LOGI(TAG, "DNS set via QIDNSCFG (Quectel)");
        return;
    }
    ESP_LOGW(TAG, "Could not set DNS – NTP may fail");
}

// ============================================================
//   AUTOMATIC APN SETUP
// ============================================================
static bool gsm_setup_apn(void) {
    ESP_LOGI(TAG, "Trying automatic APN (empty)...");
    if (send_at_cmd("AT+CGDCONT=1,\"IP\",\"\"\r\n", "OK", 2000)) {
        if (send_at_cmd("AT+CGACT=1,1\r\n", "OK", 5000)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (send_at_cmd("AT+CGPADDR=1\r\n", ".", 2000)) {
                ESP_LOGI(TAG, ">>> APN SUCCESS: Automatic (empty) APN");
                gsm_setup_dns();
                return true;
            }
        }
    }

    for (int i = 0; fallback_apns[i] != NULL; i++) {
        const char *apn = fallback_apns[i];
        ESP_LOGI(TAG, "Trying fallback APN %d: '%s'", i+1, apn);

        char cmd[64];
        snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"\r\n", apn);
        if (!send_at_cmd(cmd, "OK", 2000)) continue;
        if (!send_at_cmd("AT+CGACT=1,1\r\n", "OK", 5000)) continue;
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (send_at_cmd("AT+CGPADDR=1\r\n", ".", 2000)) {
            ESP_LOGI(TAG, ">>> APN SUCCESS: '%s'", apn);
            gsm_setup_dns();
            return true;
        }
    }
    ESP_LOGE(TAG, "All APN attempts failed");
    return false;
}

// ============================================================
//  GSM TIME SYNC
// ============================================================
bool gsm_get_time_str(char *buf, size_t buf_size) {
    ESP_LOGI(TAG, "Activating Internet for Time Sync...");

    if (!gsm_setup_apn()) return false;

    if (!send_at_cmd("AT+CGACT?\r\n", "+CGACT: 1,1", 2000)) {
        if (!send_at_cmd("AT+CGACT=1,1\r\n", "OK", 5000)) return false;
    }

    static const char *ntp_servers[] = {
        "pool.ntp.org", "216.239.35.0", "162.159.200.123", NULL
    };

    for (int s = 0; ntp_servers[s] != NULL; s++) {
        const char *server = ntp_servers[s];
        ESP_LOGI(TAG, "NTP attempt with server: %s", server);

        char ntp_cmd[128];
        snprintf(ntp_cmd, sizeof(ntp_cmd), "AT+CNTP=\"%s\",0\r\n", server);
        if (!send_at_cmd(ntp_cmd, "OK", 5000)) continue;

        int attempts = 0;
        while (attempts < 3) {
            if (send_at_cmd("AT+CNTP\r\n", "+CNTP: 0", 20000)) {
                xSemaphoreTake(gsm_mutex, portMAX_DELAY);
                uart_flush_input(GSM_UART_NUM);
                uart_write_bytes(GSM_UART_NUM, "AT+CCLK?\r\n", 10);

                uint8_t data[128];
                int total_len = 0;
                TickType_t start = xTaskGetTickCount();
                char response[64] = {0};
                bool found = false;

                while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(2000)) {
                    size_t available = 0;
                    uart_get_buffered_data_len(GSM_UART_NUM, &available);
                    if (available > 0) {
                        int len = uart_read_bytes(GSM_UART_NUM, data + total_len,
                                                 sizeof(data) - 1 - total_len,
                                                 20 / portTICK_PERIOD_MS);
                        if (len > 0) {
                            total_len += len;
                            data[total_len] = 0;
                            char *p = strstr((char *)data, "+CCLK: \"");
                            if (p) {
                                strncpy(response, p + 8, 17);
                                response[17] = 0;
                                found = true;
                                break;
                            }
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                xSemaphoreGive(gsm_mutex);

                if (found) {
                    struct tm tm = {0};
                    int yy, MM, dd, hh, mm, ss;
                    if (sscanf(response, "%d/%d/%d,%d:%d:%d", &yy, &MM, &dd, &hh, &mm, &ss) == 6) {
                        tm.tm_year = (2000 + yy) - 1900;
                        tm.tm_mon = MM - 1;
                        tm.tm_mday = dd;
                        tm.tm_hour = hh;
                        tm.tm_min = mm;
                        tm.tm_sec = ss;
                        time_t t = mktime(&tm);
                        snprintf(buf, buf_size, "%llu", (unsigned long long)t);
                        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                        settimeofday(&tv, NULL);
                        ESP_LOGI(TAG, "GSM Time Synced (UTC): %s", buf);
                        if (!gsm_registered) {
                            gsm_registered = true;
                            gsm_set_led(true);
                        }
                        return true;
                    }
                }
            }
            attempts++;
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    return false;
}

// ============================================================
//  INIT, POWER, STATUS, ABORT
// ============================================================
void gsm_init(void) {
    ESP_LOGI(TAG, "Initializing GSM...");
    gsm_mutex = xSemaphoreCreateMutex();

    uart_config_t uart_config = {
        .baud_rate = GSM_BAUD_RATE, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GSM_UART_NUM, GSM_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GSM_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(GSM_UART_NUM, GSM_TX_PIN, GSM_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << GSM_PWRKEY_PIN) | (1ULL << GSM_LED_PIN);
    gpio_config(&io_conf);

    gpio_set_level(GSM_PWRKEY_PIN, 1);
    gsm_set_led(false);
}

void gsm_power_on(void) {
    if (send_at_cmd("AT\r\n", "OK", 500)) {
        send_at_cmd("ATE0\r\n", "OK", 1000);
        return;
    }
    ESP_LOGI(TAG, "Pulsing PWRKEY...");
    gpio_set_level(GSM_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1500));
    gpio_set_level(GSM_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(5000));
    send_at_cmd("ATE0\r\n", "OK", 1000);
}

bool gsm_is_connected(void) { return gsm_registered; }

void gsm_abort_current_operation(void) {
    ESP_LOGW(TAG, "!!! ABORTING Current GSM Operation !!!");
    gsm_abort_requested = true;
    if (xSemaphoreTake(gsm_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uart_write_bytes(GSM_UART_NUM, "AT+HTTPTERM\r\n", 13);
        xSemaphoreGive(gsm_mutex);
    }
}

static bool gsm_sync_time_for_ssl(void) {
    // Obtain current UTC time from the network (GSM) and write to modem’s clock
    char time_str[32];
    if (!gsm_get_time_str(time_str, sizeof(time_str))) {
        ESP_LOGW(TAG, "Could not get GSM time for SSL, trying NTP...");
        // Alternative: use WiFi NTP if available
    }
    // GSM time format is Unix timestamp string. Convert to "YY/MM/DD,HH:MM:SS"
    time_t t = (time_t)atol(time_str);
    struct tm *utc = gmtime(&t);
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CCLK=\"%02d/%02d/%02d,%02d:%02d:%02d\"\r\n",
             utc->tm_year - 100, utc->tm_mon + 1, utc->tm_mday,
             utc->tm_hour, utc->tm_min, utc->tm_sec);
    return send_at_cmd(cmd, "OK", 2000);
}

// ============================================================
//  HTTP POST
// ============================================================
bool gsm_http_post(const char *url, const char *json_data) {
    int attempts = 0;
    const int max_retries = 3;
    gsm_abort_requested = false;

    while (attempts < max_retries) {
        if (gsm_abort_requested) return false;
        ESP_LOGI(TAG, "HTTP POST Attempt %d/%d...", attempts + 1, max_retries);
        send_at_cmd("AT+HTTPTERM\r\n", "OK", 500);

        if (!gsm_setup_apn()) { attempts++; continue; }
        if (!send_at_cmd("AT+CGPADDR=1\r\n", ".", 2000)) { attempts++; continue; }
        if (!send_at_cmd("AT+HTTPINIT\r\n", "OK", 2000)) { attempts++; continue; }
        
if (strncmp(url, "https", 5) == 0) send_at_cmd("AT+HTTPPARA=\"HTTPS\",1\r\n", "OK", 2000);

        char cmd[512];
        snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=\"URL\",\"%s\"\r\n", url);
        if (!send_at_cmd(cmd, "OK", 2000)) goto retry;

        if (!send_at_cmd("AT+HTTPPARA=\"CONTENT\",\"application/json\"\r\n", "OK", 2000)) goto retry;

        snprintf(cmd, sizeof(cmd), "AT+HTTPDATA=%d,12000\r\n", strlen(json_data));
        if (!send_at_cmd(cmd, "DOWNLOAD", 6000)) goto retry;

        xSemaphoreTake(gsm_mutex, portMAX_DELAY);
        uart_write_bytes(GSM_UART_NUM, json_data, strlen(json_data));
        xSemaphoreGive(gsm_mutex);

        vTaskDelay(pdMS_TO_TICKS(500));
        if (!send_at_cmd(NULL, "OK", 8000)) goto retry;

        ESP_LOGI(TAG, "Executing POST...");
        xSemaphoreTake(gsm_mutex, portMAX_DELAY);
        uart_flush_input(GSM_UART_NUM);
        uart_write_bytes(GSM_UART_NUM, "AT+HTTPACTION=1\r\n", 17);

        uint8_t rx_buf[128];
        int total_len = 0;
        bool action_success = false;
        TickType_t start = xTaskGetTickCount();

        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(30000)) {
            if (gsm_abort_requested) {
                xSemaphoreGive(gsm_mutex);
                return false;
            }
            size_t available = 0;
            uart_get_buffered_data_len(GSM_UART_NUM, &available);
            if (available > 0) {
                int len = uart_read_bytes(GSM_UART_NUM, rx_buf + total_len,
                                         sizeof(rx_buf) - 1 - total_len,
                                         20 / portTICK_PERIOD_MS);
                if (len > 0) {
                    total_len += len;
                    rx_buf[total_len] = 0;
                    if (strstr((char*)rx_buf, "+HTTPACTION: 1,200") ||
                        strstr((char*)rx_buf, "+HTTPACTION: 1,201")) {
                        action_success = true;
                        ESP_LOGI(TAG, "HTTP POST Success (200/201)");
                        break;
                    }
                    if (strstr((char*)rx_buf, "ERROR") ||
                        strstr((char*)rx_buf, "+HTTPACTION: 1,60")) break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        xSemaphoreGive(gsm_mutex);

        if (action_success) {
            send_at_cmd("AT+HTTPTERM\r\n", "OK", 1000);
            return true;
        }

    retry:
        ESP_LOGW(TAG, "Attempt %d failed. Retrying...", attempts + 1);
        send_at_cmd("AT+HTTPTERM\r\n", "OK", 1000);
        attempts++;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    ESP_LOGE(TAG, "All HTTP attempts failed.");
    return false;
}

// ============================================================
//  HTTP GET JSON 
// ============================================================
bool gsm_http_get_json(const char *url, char *response, size_t size) {
    if (gsm_mutex == NULL) return false;
    memset(response, 0, size);

    if (!gsm_setup_apn()) return false;

    if (!send_at_cmd("AT+HTTPINIT\r\n", "OK", 5000)) goto fail;
    
    // Disable HTTPSSL requirement so Netlify/Azure works automatically
    if (strncmp(url, "https", 5) == 0) send_at_cmd("AT+HTTPSSL=1\r\n", "OK", 2000);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=\"URL\",\"%s\"\r\n", url);
    if (!send_at_cmd(cmd, "OK", 2000)) goto fail;

    xSemaphoreTake(gsm_mutex, portMAX_DELAY);
    uart_flush_input(GSM_UART_NUM);
    uart_write_bytes(GSM_UART_NUM, "AT+HTTPACTION=0\r\n", 17);

    uint8_t rx[512];
    int total = 0;
    int http_status = 0;
    size_t content_length = 0;
    bool action_done = false;
    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(30000)) {
        size_t avail = 0;
        uart_get_buffered_data_len(GSM_UART_NUM, &avail);
        if (avail > 0) {
            int len = uart_read_bytes(GSM_UART_NUM, rx + total,
                                     sizeof(rx) - 1 - total, 100 / portTICK_PERIOD_MS);
            if (len > 0) {
                total += len;
                rx[total] = 0;
                char *ptr = strstr((char*)rx, "+HTTPACTION: 0,");
                if (ptr) {
                    sscanf(ptr, "+HTTPACTION: 0,%d,%zu", &http_status, &content_length);
                    action_done = true;
                    break;
                }
                if (strstr((char*)rx, "ERROR")) break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    xSemaphoreGive(gsm_mutex);

    if (!action_done || http_status != 200) goto fail;

    if (content_length == 0) content_length = 512;
    if (content_length > size) content_length = size - 1;

    xSemaphoreTake(gsm_mutex, portMAX_DELAY);
    snprintf(cmd, sizeof(cmd), "AT+HTTPREAD=0,%d\r\n", (int)content_length);
    uart_flush_input(GSM_UART_NUM);
    uart_write_bytes(GSM_UART_NUM, cmd, strlen(cmd));

    char raw_buf[768] = {0};
    total = 0;
    start = xTaskGetTickCount();
    bool done = false;
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(15000)) {
        size_t avail = 0;
        uart_get_buffered_data_len(GSM_UART_NUM, &avail);
        if (avail > 0) {
            int len = uart_read_bytes(GSM_UART_NUM, (uint8_t*)(raw_buf + total),
                                     sizeof(raw_buf) - 1 - total, 20 / portTICK_PERIOD_MS);
            if (len > 0) {
                total += len;
                raw_buf[total] = 0;
                if (total >= content_length) { done = true; break; }
            }
        } else if (total > 0) {
            done = true; break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    xSemaphoreGive(gsm_mutex);

    if (!done && total == 0) goto fail;

    char *json_start = strchr(raw_buf, '{');
    if (!json_start) goto fail;

    strncpy(response, json_start, size - 1);
    response[size - 1] = '\0';
    ESP_LOGI(TAG, "GSM GET JSON success: %s", response);

    send_at_cmd("AT+HTTPTERM\r\n", "OK", 2000);
    return true;

fail:
    send_at_cmd("AT+HTTPTERM\r\n", "OK", 2000);
    return false;
}

// ============================================================
//  MONITOR TASK
// ============================================================
static void gsm_monitor_task(void *pvParameters) {
    int fail_count = 0;
    while (1) {
        if (gsm_ota_busy) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!gsm_abort_requested) {
            bool is_alive = send_at_cmd("AT\r\n", "OK", 1000);
            if (!is_alive) {
                ESP_LOGE(TAG, ">>> CRITICAL: GSM Module NOT RESPONDING! <<<");
                gsm_registered = false;
                gsm_set_led(false);
                fail_count++;
                if (fail_count > 3) { gsm_power_on(); fail_count = 0; }
            } else {
                if (!send_at_cmd("AT+CPIN?\r\n", "+CPIN: READY", 1000)) {
                    ESP_LOGE(TAG, "SIM Card Not Ready! Forcing HARD REBOOT...");
                    gsm_registered = false;
                    gsm_set_led(false);
                    gpio_set_level(GSM_PWRKEY_PIN, 0); vTaskDelay(pdMS_TO_TICKS(2000));
                    gpio_set_level(GSM_PWRKEY_PIN, 1); vTaskDelay(pdMS_TO_TICKS(3000));
                    gpio_set_level(GSM_PWRKEY_PIN, 0); vTaskDelay(pdMS_TO_TICKS(2000));
                    gpio_set_level(GSM_PWRKEY_PIN, 1); vTaskDelay(pdMS_TO_TICKS(5000));
                    fail_count = 0;
                } else {
                    fail_count = 0;
                    bool reg = send_at_cmd("AT+CGPADDR=1\r\n", ".", 1000);
                    if (reg != gsm_registered) {
                        gsm_registered = reg;
                        gsm_set_led(reg);
                        ESP_LOGI(TAG, "GSM Network Status: %s", reg ? "Connected" : "Disconnected");
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

void gsm_start_monitoring(void) {
    xTaskCreate(gsm_monitor_task, "gsm_mon", 4096, NULL, 5, NULL);
}

// ============================================================
//  OTA DOWNLOAD (4KB RAM BUFFER & OFFSET MATH FIX)
// ============================================================
// ============================================================
//  OTA DOWNLOAD (UPDATED FOR SIMCOM A7677S HTTPS)
// ============================================================
// ============================================================
//  OTA DOWNLOAD (UPDATED FOR SIMCOM A7677S HTTPS)
// ============================================================
bool gsm_ota_download_to_partition(const char *url, esp_ota_handle_t ota_handle, size_t *out_size) {
    if (gsm_mutex == NULL) return false;

    const size_t CHUNK_SIZE = 4096; 
    uint8_t *chunk_buf = malloc(CHUNK_SIZE);
    if (!chunk_buf) {
        ESP_LOGE(TAG, "OOM: Failed to allocate 4KB OTA RAM buffer!");
        return false;
    }

    gsm_ota_busy = true;   
    ESP_LOGI(TAG, "GSM OTA download (A7677S Optimized): %s", url);

    if (!gsm_setup_apn()) {
        ESP_LOGE(TAG, "APN setup failed");
        gsm_ota_busy = false;
        free(chunk_buf);
        return false;
    }

    size_t written = 0;
    size_t total_file_size = 0;
    int retries = 0;
    const int max_retries = 30;

    while (retries < max_retries) {
        send_at_cmd("AT+HTTPTERM\r\n", "OK", 1000);
        vTaskDelay(pdMS_TO_TICKS(500));

        if (!send_at_cmd("AT+HTTPINIT\r\n", "OK", 5000)) {
            gsm_setup_apn(); 
            retries++; 
            vTaskDelay(pdMS_TO_TICKS(3000)); 
            continue;
        }
        
        // --- A7677S SPECIFIC HTTPS CONFIGURATION ---
        if (strncmp(url, "https", 5) == 0) {
            // DO NOT use AT+HTTPSSL=1 here. It will cause an ERROR on A7677S.
            
            // Set SSL version to TLS 1.2 for Context 0
            send_at_cmd("AT+CSSLCFG=\"sslversion\",0,4\r\n", "OK", 1000);
            
            // Set authmode to 0 (No certificate verification) for Context 0
            send_at_cmd("AT+CSSLCFG=\"authmode\",0,0\r\n", "OK", 1000);
            
            // Bypass RTC time check (context 0, value 1)
            send_at_cmd("AT+CSSLCFG=\"ignorertctime\",0,1\r\n", "OK", 1000);
            
            // IMPORTANT: Link the HTTP session to SSL Context 0 (This enables HTTPS)
            send_at_cmd("AT+HTTPPARA=\"SSLCFG\",0\r\n", "OK", 1000);
        }

        char cmd[512];
        snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=\"URL\",\"%s\"\r\n", url);
        if (!send_at_cmd(cmd, "OK", 2000)) goto session_retry;

        // Resume support
        if (written > 0) {
            snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=\"USERDATA\",\"Range: bytes=%u-\"\r\n", (unsigned int)written);
            send_at_cmd(cmd, "OK", 2000);
        }

        xSemaphoreTake(gsm_mutex, portMAX_DELAY);
        uart_flush_input(GSM_UART_NUM);
        uart_write_bytes(GSM_UART_NUM, "AT+HTTPACTION=0\r\n", 17);

        uint8_t rx[512];
        int total = 0;
        int http_status = 0;
        size_t content_length = 0;
        bool action_done = false;
        TickType_t start = xTaskGetTickCount();

        while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(60000)) {
            size_t avail = 0;
            uart_get_buffered_data_len(GSM_UART_NUM, &avail);
            if (avail > 0) {
                int len = uart_read_bytes(GSM_UART_NUM, rx + total, sizeof(rx) - 1 - total, pdMS_TO_TICKS(50));
                if (len > 0) {
                    total += len;
                    rx[total] = 0;
                    char *ptr = strstr((char*)rx, "+HTTPACTION: 0,");
                    if (ptr) {
                        sscanf(ptr, "+HTTPACTION: 0,%d,%zu", &http_status, &content_length);
                        action_done = true;
                        break;
                    }
                    if (strstr((char*)rx, "ERROR")) break;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        xSemaphoreGive(gsm_mutex);

        if (!action_done || (http_status != 200 && http_status != 206) || content_length == 0) {
            ESP_LOGE(TAG, "Action failed. Status=%d", http_status);
            goto session_retry;
        }

        if (http_status == 200) {
            if (written > 0) {
                send_at_cmd("AT+HTTPTERM\r\n", "OK", 2000);
                gsm_ota_busy = false;
                free(chunk_buf);
                return false; 
            }
            total_file_size = content_length;
        } else if (http_status == 206) {
            if (total_file_size == 0) total_file_size = written + content_length;
        }

        size_t session_remaining = content_length;
        while (session_remaining > 0) {
            size_t request = (session_remaining > CHUNK_SIZE) ? CHUNK_SIZE : session_remaining;
            size_t session_read_offset = content_length - session_remaining;
            
            snprintf(cmd, sizeof(cmd), "AT+HTTPREAD=%u,%u\r\n", (unsigned int)session_read_offset, (unsigned int)request);

            xSemaphoreTake(gsm_mutex, portMAX_DELAY);
            uart_flush_input(GSM_UART_NUM);   
            uart_write_bytes(GSM_UART_NUM, cmd, strlen(cmd));

            char line[64];
            int line_len = 0;
            int reported_len = 0;
            bool header_found = false;
            start = xTaskGetTickCount();

            while (!header_found && (xTaskGetTickCount() - start) < pdMS_TO_TICKS(20000)) { 
                uint8_t ch;
                if (uart_read_bytes(GSM_UART_NUM, &ch, 1, pdMS_TO_TICKS(10)) == 1) {
                    if (ch == '\n') {
                        line[line_len] = '\0';
                        if (strncmp(line, "+HTTPREAD:", 10) == 0) {
                            sscanf(line, "+HTTPREAD: %d", &reported_len);
                            header_found = true;
                        } else if (strstr(line, "ERROR")) break;
                        else line_len = 0; 
                    } else if (ch != '\r' && line_len < sizeof(line) - 1) {
                        line[line_len++] = ch;
                    }
                }
            }

            if (!header_found) { xSemaphoreGive(gsm_mutex); goto session_retry; }
            if (reported_len <= 0) { xSemaphoreGive(gsm_mutex); vTaskDelay(pdMS_TO_TICKS(500)); continue; }

            int bytes_left = reported_len;
            int buf_idx = 0;
            while (bytes_left > 0) {
                int n = uart_read_bytes(GSM_UART_NUM, chunk_buf + buf_idx, bytes_left, pdMS_TO_TICKS(2000));
                if (n > 0) { buf_idx += n; bytes_left -= n; }
            }
            xSemaphoreGive(gsm_mutex);
            
            esp_err_t err = esp_ota_write(ota_handle, chunk_buf, reported_len);
            if (err != ESP_OK) {
                send_at_cmd("AT+HTTPTERM\r\n", "OK", 2000);
                gsm_ota_busy = false; free(chunk_buf); return false; 
            }

            written += reported_len;
            session_remaining -= reported_len;
        }

        if (session_remaining == 0) {
            send_at_cmd("AT+HTTPTERM\r\n", "OK", 2000);
            *out_size = written;
            gsm_ota_busy = false;
            free(chunk_buf);
            return true;
        }

session_retry:
        send_at_cmd("AT+HTTPTERM\r\n", "OK", 2000);
        retries++;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    gsm_ota_busy = false; free(chunk_buf); return false;
}

bool gsm_is_ota_busy(void) {
    return gsm_ota_busy;
}
