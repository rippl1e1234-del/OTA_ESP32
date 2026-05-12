#include "data_reporter.h"
#include "gsm_manager.h"
#include "time_manager.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_crt_bundle.h" 
#include "cJSON.h" 

static const char *TAG = "DATA_REP";

// --- UAT (TESTING) URL ---
//#define SERVER_URL "https://ripple-auth-api-uat.azurewebsites.net/v1/LeakDetectingDevice/create-device-communication"
// --- PROD URL ---
#define SERVER_URL "https://ripple-auth-app-prod.azurewebsites.net/v1/LeakDetectingDevice/create-device-communication"

//#define SERVER_URL "http://rippletestvm.centralindia.cloudapp.azure.com:9000/add-gasleak-info"

extern volatile bool wifi_connected;

static void get_uid(char *buf, size_t size) {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    // UID Format: SGLD + Last 3 MAC bytes (Reversed [5][4][3])
    snprintf(buf, size, "SGLD%02X%02X%02X", mac[5], mac[4], mac[3]);
}

static bool send_wifi_post(const char *json_data) {
    ESP_LOGI(TAG, "Sending via Wi-Fi...");
    
    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach, 
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP Client");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_data, strlen(json_data));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    
    if (err == ESP_OK) {
        // Debug Response
        int content_len = esp_http_client_get_content_length(client);
        if (content_len < 0) content_len = 512; 
        char *response_buf = (char *)malloc(content_len + 1);
        if (response_buf) {
            int read_len = esp_http_client_read_response(client, response_buf, content_len);
            if (read_len > 0) {
                response_buf[read_len] = 0; 
                ESP_LOGI(TAG, "SERVER RESPONSE: %s", response_buf);
            }
            free(response_buf);
        }
    }

    esp_http_client_cleanup(client);

    if (err == ESP_OK && (status_code == 200 || status_code == 201)) {
        ESP_LOGI(TAG, "Wi-Fi POST Success (Code: %d)", status_code);
        return true;
    } else {
        ESP_LOGE(TAG, "Wi-Fi POST Failed (Err: %s, Status: %d)", esp_err_to_name(err), status_code);
        return false;
    }
}

// --- INTERNAL SHARED SENDER ---
static bool send_packet_internal(const char *message, bool is_emergency) {
    // 1. Prepare Data
    char timestamp[32];
    time_get_timestamp_str(timestamp, sizeof(timestamp));

    // --- FALLBACK LOGIC ---
    if (atol(timestamp) < 1735689600) {
        ESP_LOGW(TAG, "System Time Invalid. Trying GSM Time...");
        if (!gsm_get_time_str(timestamp, sizeof(timestamp))) {
             ESP_LOGE(TAG, "Failed to get time from GSM too. Using default.");
        }
    }
    // ----------------------

    char uid[16];
    get_uid(uid, sizeof(uid));

    const char *w_stat = wifi_connected ? "connected" : "disconnected";
    const char *g_stat = gsm_is_connected() ? "connected" : "disconnected";

    // 2. Build JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "timestamp", timestamp);
    cJSON_AddStringToObject(root, "wifi_status", w_stat);
    cJSON_AddStringToObject(root, "gsm_status", g_stat);
    cJSON_AddStringToObject(root, "uniqueId", uid);
    cJSON_AddStringToObject(root, "message", message);

    char *json_str = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG, ">>> GENERATED PACKET (%s): %s", is_emergency ? "EMERGENCY" : "STATUS", json_str);

    bool success = false;

    // 3. Send Logic
    // Try Wi-Fi first
    if (wifi_connected) {
        success = send_wifi_post(json_str);
    }

    // Failover to GSM
    if (!success) {
        if (is_emergency) {
            ESP_LOGW(TAG, "EMERGENCY: Requesting GSM Priority...");
            // TODO: Uncomment in Phase 3 after updating GSM Manager
            gsm_abort_current_operation(); 
        }

        if (gsm_is_connected()) {
            ESP_LOGW(TAG, "Switching to GSM...");
            success = gsm_http_post(SERVER_URL, json_str);
        } else {
            ESP_LOGE(TAG, "NO CONNECTION AVAILABLE FOR PACKET!");
        }
    }

    free(json_str);
    cJSON_Delete(root);
    return success;
}

// --- PUBLIC FUNCTIONS ---

bool data_reporter_send_ready(void) {
    return send_packet_internal("Device Ready", false);
}

bool data_reporter_send_status(void) {
    return send_packet_internal("no issue", false);
}

bool data_reporter_send_emergency(int gas_value) {
    // Note: We currently send the standard string message.
    // The gas_value can be logged or added to JSON if API supports it later.
    ESP_LOGE(TAG, "Preparing Emergency Packet (Value: %d)", gas_value);
    return send_packet_internal("Gas Leak Detected", true);
}


