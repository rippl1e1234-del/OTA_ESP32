// ota_manager.c: Thread-safe MQTT OTA with TLS (Wi-Fi primary, GSM fallback ready)
#include "ota_manager.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "NeonLED.h"
#include "gas_sensor.h"
#include "gsm_manager.h"   // Included for GSM download functions
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "OTA_MQTT";

// MQTT topics
#define OTA_COMMAND_TOPIC     "esp32/ota/command"
#define OTA_CHUNK_TOPIC       "esp32/ota/chunk"
#define OTA_GSM_COMMAND_TOPIC "esp32/ota/gsm_command" // Dedicated topic for GSM HTTPS trigger

// OTA state machine
typedef enum {
    OTA_IDLE,
    OTA_WAITING_START,   // connected, waiting for command
    OTA_DOWNLOADING,     // receiving firmware chunks via Wi-Fi
    OTA_GSM_DOWNLOADING  // Async GSM task is handling it
} ota_state_t;

// ----- OTA global state (protected by ota_mutex) -----
static ota_state_t ota_state = OTA_IDLE;
static char ota_checksum[65] = {0};
static size_t ota_total_size = 0;
static size_t ota_bytes_received = 0;
static esp_ota_handle_t ota_handle = 0;
static const esp_partition_t *ota_partition = NULL;
static SemaphoreHandle_t ota_mutex = NULL;

// Timeout handling
static TimerHandle_t ota_chunk_timer = NULL;
#define OTA_CHUNK_TIMEOUT_MS    (5 * 60 * 1000)   // 5 minutes

// MQTT client handle
static esp_mqtt_client_handle_t mqtt_client = NULL;

// ===============================================================
// HELPER FUNCTIONS (Must be defined before they are used)
// ===============================================================

static void ota_lock(void) {
    if (ota_mutex) xSemaphoreTake(ota_mutex, portMAX_DELAY);
}

static void ota_unlock(void) {
    if (ota_mutex) xSemaphoreGive(ota_mutex);
}

// Verify SHA‑256 of a whole partition
static bool verify_partition_sha256(const esp_partition_t *part,
                                    const char *expected_hash_hex,
                                    size_t hash_len,
                                    size_t data_len)
{
    if (!part || !expected_hash_hex) return false;

    unsigned char expected[32], computed[32];
    for (size_t i = 0; i < 32; i++) {
        unsigned int byte;
        if (sscanf(expected_hash_hex + 2*i, "%2x", &byte) != 1) return false;
        expected[i] = (unsigned char)byte;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);   // SHA-256

    uint8_t buf[512];
    size_t offset = 0;
    // Hash only the actual data written
    size_t part_size = (data_len > 0) ? data_len : part->size;
    while (offset < part_size) {
        size_t to_read = part_size - offset;
        if (to_read > sizeof(buf)) to_read = sizeof(buf);
        if (esp_partition_read(part, offset, buf, to_read) != ESP_OK) {
            mbedtls_sha256_free(&ctx);
            return false;
        }
        mbedtls_sha256_update(&ctx, buf, to_read);
        offset += to_read;
    }
    mbedtls_sha256_finish(&ctx, computed);
    mbedtls_sha256_free(&ctx);

    return (memcmp(computed, expected, 32) == 0);
}

// Abort current OTA (must be called with mutex held)
static void ota_abort_locked(const char *reason) {
    ESP_LOGE(TAG, "%s - aborting OTA", reason);
    if (ota_handle) {
        esp_ota_end(ota_handle);
        ota_handle = 0;
    }
    ota_state = OTA_WAITING_START;
    ota_bytes_received = 0;
    ota_checksum[0] = '\0';

    if (ota_chunk_timer) {
        xTimerStop(ota_chunk_timer, 0);
    }

    neon_set_status(NEON_FAIL);
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (!gas_is_emergency()) neon_set_status(NEON_STABLE);
}

// Public wrapper (takes lock)
static void ota_abort(const char *reason) {
    ota_lock();
    ota_abort_locked(reason);
    ota_unlock();
}

// ===============================================================
// GSM ASYNCHRONOUS TASK
// ===============================================================

typedef struct {
    char url[256];
    char version[32];
    char checksum[65];
    size_t size;
} gsm_ota_task_params_t;

static void gsm_https_ota_task(void *pvParameters) {
    gsm_ota_task_params_t *params = (gsm_ota_task_params_t *)pvParameters;
    
    ESP_LOGI(TAG, "Starting Async GSM OTA Task for Version: %s", params->version);
    neon_set_status(NEON_CONNECTING);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found!");
        goto gsm_task_cleanup;
    }

    esp_ota_handle_t update_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        goto gsm_task_cleanup;
    }

    // Call the GSM HTTP Downloader from gsm_manager.c
    size_t downloaded_size = 0;
    bool success = gsm_ota_download_to_partition(params->url, update_handle, &downloaded_size);

    if (!success) {
        ESP_LOGE(TAG, "GSM HTTPS Download failed.");
        esp_ota_abort(update_handle);
        goto gsm_task_cleanup;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        goto gsm_task_cleanup;
    }

    // Validation
    if (downloaded_size != params->size) {
        ESP_LOGE(TAG, "Size mismatch! Expected %u, got %u", (unsigned)params->size, (unsigned)downloaded_size);
        goto gsm_task_cleanup;
    }
    
    if (!verify_partition_sha256(update_partition, params->checksum, 64, downloaded_size)) {
        ESP_LOGE(TAG, "Checksum mismatch! Firmware corrupt.");
        goto gsm_task_cleanup;
    }

    // Set boot partition and reboot
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Boot partition set failed: %s", esp_err_to_name(err));
        goto gsm_task_cleanup;
    }

    ESP_LOGI(TAG, "GSM HTTPS OTA Successful! Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

gsm_task_cleanup:
    ESP_LOGW(TAG, "GSM OTA Task Failed. Cleaning up...");
    neon_set_status(NEON_FAIL);
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (!gas_is_emergency()) neon_set_status(NEON_STABLE);
    
    ota_lock();
    ota_state = OTA_IDLE;
    ota_unlock();
    
    free(params);
    vTaskDelete(NULL);
}

// ===============================================================
// WIFI CHUNK HANDLER
// ===============================================================

static void ota_handle_chunk(const uint8_t *data, size_t len) {
    if (ota_state != OTA_DOWNLOADING) {
        ESP_LOGW(TAG, "Received chunk while not downloading, ignoring");
        return;
    }

    if (ota_bytes_received + len > ota_total_size) {
        ota_abort_locked("Chunk overflow");
        return;
    }

    esp_err_t err = esp_ota_write(ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        ota_abort_locked("Flash write error");
        return;
    }

    ota_bytes_received += len;
    ESP_LOGI(TAG, "Received chunk: %u bytes, total %u/%u",
             (unsigned)len, (unsigned)ota_bytes_received, (unsigned)ota_total_size);

    if (ota_bytes_received == ota_total_size) {
        ESP_LOGI(TAG, "All firmware bytes received. Verifying checksum...");

        err = esp_ota_end(ota_handle);
        ota_handle = 0;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            ota_abort_locked("OTA end error");
            return;
        }

        if (!verify_partition_sha256(ota_partition, ota_checksum, 64, ota_total_size)) {
            ESP_LOGE(TAG, "Checksum mismatch! Firmware corrupt.");
            ota_abort_locked("Checksum mismatch");
            return;
        }

        err = esp_ota_set_boot_partition(ota_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
            ota_abort_locked("Boot partition set failed");
            return;
        }

        ESP_LOGI(TAG, "MQTT OTA successful. Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
}

// ===============================================================
// MQTT EVENTS
// ===============================================================

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            ota_lock();
            ota_state = OTA_WAITING_START;
            ota_unlock();
            
            esp_mqtt_client_subscribe(mqtt_client, OTA_COMMAND_TOPIC, 1);
            esp_mqtt_client_subscribe(mqtt_client, OTA_GSM_COMMAND_TOPIC, 1); 
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            ota_lock();
            if (ota_state == OTA_DOWNLOADING) {
                ota_abort_locked("MQTT disconnected during Wi-Fi OTA");
            } else if (ota_state == OTA_GSM_DOWNLOADING) {
                ESP_LOGI(TAG, "MQTT disconnected, but GSM OTA is running. Ignoring.");
            } else {
                ota_state = OTA_IDLE;
            }
            ota_unlock();
            break;

        case MQTT_EVENT_DATA: {
            if (event->topic_len == 0 || event->data_len == 0) break;

            // Wi-Fi CHUNK OTA COMMAND
            if (strncmp(event->topic, OTA_COMMAND_TOPIC, event->topic_len) == 0) {
                char *payload = malloc(event->data_len + 1);
                if (!payload) break;
                memcpy(payload, event->data, event->data_len);
                payload[event->data_len] = '\0';

                cJSON *json = cJSON_Parse(payload);
                free(payload);
                if (!json) break;

                cJSON *ver = cJSON_GetObjectItem(json, "version");
                cJSON *cks = cJSON_GetObjectItem(json, "checksum");
                cJSON *sz  = cJSON_GetObjectItem(json, "size");

                if (!cJSON_IsString(ver) || !cJSON_IsString(cks) || !cJSON_IsNumber(sz)) {
                    cJSON_Delete(json);
                    break;
                }

                const esp_app_desc_t *app = esp_app_get_description();
                if (strcmp(app->version, ver->valuestring) == 0) {
                    cJSON_Delete(json);
                    break;
                }

                ota_lock();
                if (ota_state != OTA_WAITING_START) {
                    ota_unlock();
                    cJSON_Delete(json);
                    break;
                }

                ota_total_size = (size_t)sz->valuedouble;
                strncpy(ota_checksum, cks->valuestring, sizeof(ota_checksum)-1);
                ota_checksum[sizeof(ota_checksum)-1] = '\0';
                cJSON_Delete(json);

                ota_partition = esp_ota_get_next_update_partition(NULL);
                esp_ota_begin(ota_partition, ota_total_size, &ota_handle);
                ota_bytes_received = 0;
                ota_state = OTA_DOWNLOADING;
                neon_set_status(NEON_CONNECTING);
                ota_unlock();

                esp_mqtt_client_subscribe(mqtt_client, OTA_CHUNK_TOPIC, 1);
                if (ota_chunk_timer) xTimerStart(ota_chunk_timer, 0);
            }
            // Wi-Fi CHUNK DATA
            else if (strncmp(event->topic, OTA_CHUNK_TOPIC, event->topic_len) == 0) {
                ota_lock();
                if (ota_state == OTA_DOWNLOADING) {
                    ota_handle_chunk((const uint8_t *)event->data, event->data_len);
                    if (ota_chunk_timer) xTimerReset(ota_chunk_timer, 0);
                }
                ota_unlock();
            }
            // NEW: GSM HTTPS OTA COMMAND
            else if (strncmp(event->topic, OTA_GSM_COMMAND_TOPIC, event->topic_len) == 0) {
                char *payload = malloc(event->data_len + 1);
                if (!payload) break;
                memcpy(payload, event->data, event->data_len);
                payload[event->data_len] = '\0';

                cJSON *json = cJSON_Parse(payload);
                free(payload);
                if (!json) break;

                cJSON *ver = cJSON_GetObjectItem(json, "version");
                cJSON *url = cJSON_GetObjectItem(json, "url");
                cJSON *cks = cJSON_GetObjectItem(json, "checksum");
                cJSON *sz  = cJSON_GetObjectItem(json, "size");

                if (!cJSON_IsString(ver) || !cJSON_IsString(url) || !cJSON_IsString(cks) || !cJSON_IsNumber(sz)) {
                    cJSON_Delete(json);
                    break;
                }

                const esp_app_desc_t *app = esp_app_get_description();
                if (strcmp(app->version, ver->valuestring) == 0) {
                    cJSON_Delete(json);
                    break;
                }

                ota_lock();
                if (ota_state != OTA_IDLE && ota_state != OTA_WAITING_START) {
                    ota_unlock();
                    cJSON_Delete(json);
                    break;
                }
                ota_state = OTA_GSM_DOWNLOADING;
                ota_unlock();

                gsm_ota_task_params_t *task_params = malloc(sizeof(gsm_ota_task_params_t));
                if (task_params) {
                    strncpy(task_params->url, url->valuestring, sizeof(task_params->url) - 1);
                    strncpy(task_params->version, ver->valuestring, sizeof(task_params->version) - 1);
                    strncpy(task_params->checksum, cks->valuestring, sizeof(task_params->checksum) - 1);
                    task_params->size = (size_t)sz->valuedouble;
                    xTaskCreate(gsm_https_ota_task, "gsm_https_ota", 6144, task_params, 5, NULL);
                } else {
                    ota_lock(); ota_state = OTA_IDLE; ota_unlock();
                }
                cJSON_Delete(json);
            }
            break;
        }
        default:
            break;
    }
}

static void ota_chunk_timeout_cb(TimerHandle_t xTimer) {
    ota_lock();
    if (ota_state == OTA_DOWNLOADING) ota_abort_locked("Chunk timeout");
    ota_unlock();
}

bool ota_init_mqtt(const char *broker_uri, const char *ca_cert_pem) {
    if (mqtt_client) return true;

    if (!ota_mutex) {
        ota_mutex = xSemaphoreCreateMutex();
        if (!ota_mutex) return false;
    }

    if (!ota_chunk_timer) {
        ota_chunk_timer = xTimerCreate("ota_chunk_tmo", pdMS_TO_TICKS(OTA_CHUNK_TIMEOUT_MS), pdFALSE, NULL, ota_chunk_timeout_cb);
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = broker_uri,
        .credentials.username = "Ripple",
        .credentials.authentication.password = "Ripple1234",
        .network.disable_auto_reconnect = false,
        .buffer.size = 5120,
        .buffer.out_size = 5120,
        .broker.verification.crt_bundle_attach = ca_cert_pem ? NULL : esp_crt_bundle_attach,
        .broker.verification.certificate = ca_cert_pem,
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    return true;
}

void ota_deinit_mqtt(void) {
    if (mqtt_client) {
        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }
    ota_lock();
    ota_state = OTA_IDLE;
    ota_unlock();
}
