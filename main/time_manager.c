#include "time_manager.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

static const char *TAG = "TIME_MGR";
static time_t last_ntp_sync_time = 0; 

// NEW: Check if Wi-Fi is connected
bool wifi_is_connected(void) {
    wifi_ap_record_t ap_info;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
    
    if (err == ESP_OK) {
        return true;
    } else if (err == ESP_ERR_WIFI_NOT_CONNECT) {
        return false;
    } else {
        ESP_LOGE(TAG, "Failed to get Wi-Fi connection status: %s", esp_err_to_name(err));
        return false;
    }
}

// Ping Google to check internet connectivity
bool ping_google(void) {
    int sock = -1;
    struct sockaddr_in server_addr;
    struct timeval timeout;
    bool result = false;
    
    // Create socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket for ping: %s", strerror(errno));
        return false;
    }
    
    // Set timeout (5 seconds)
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    
    // Configure server address (Google DNS: 8.8.8.8 on port 53)
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(53); // DNS port
    server_addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    
    // Try to connect
    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
        ESP_LOGI(TAG, "Ping successful: Connected to 8.8.8.8:53");
        result = true;
    } else {
        // Try alternative: Google.com on port 80
        server_addr.sin_port = htons(80);
        server_addr.sin_addr.s_addr = inet_addr("142.250.191.206"); // Google.com IP
        
        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
            ESP_LOGI(TAG, "Ping successful: Connected to google.com:80");
            result = true;
        } else {
            ESP_LOGW(TAG, "Ping failed: Could not connect to any test server");
            result = false;
        }
    }
    
    // Close socket
    close(sock);
    return result;
}

void time_manager_init(void) {
    ESP_LOGI(TAG, "Initializing SNTP...");
    
    // STOP first if it was already running to avoid "already initialized" errors
    esp_sntp_stop();

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

bool time_sync(void) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // 1. SMART RTC CHECK
    if (timeinfo.tm_year >= (2026 - 1900)) {
        
        // Handle Soft Reboot (Trust RTC)
        if (last_ntp_sync_time == 0) {
            last_ntp_sync_time = now;
            char strftime_buf[64];
            strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
            ESP_LOGI(TAG, "RTC Valid on Boot. Trusting System Time: %s", strftime_buf);
            return true;
        }

        // Daily Resync Check (86400 seconds = 24 Hours)
        if ((now - last_ntp_sync_time) < 86400) {
            return true; 
        }
        
        ESP_LOGW(TAG, "Daily Timer Expired (>24h). Forcing Resync...");
    } else {
        ESP_LOGI(TAG, "Time Invalid (Year < 2025). Starting SNTP Sync...");
    }

    // 2. ATTEMPT SNTP SYNC (One-Shot, 60 Seconds Timeout)
    // Restart SNTP if it is in RESET state
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        time_manager_init(); // This now calls esp_sntp_stop() internally
    }

    int retry = 0;
    const int max_retries = 60; // Wait up to 60 seconds
    
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry <= max_retries) {
        if (retry % 5 == 0) {
            ESP_LOGI(TAG, "Waiting for system time... (%ds/%ds)", retry, max_retries);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (retry > max_retries) {
        ESP_LOGE(TAG, "SNTP Sync Timed Out (60s). Moving to fallback.");
        return false;
    }

    // 3. SUCCESS
    time(&now);
    last_ntp_sync_time = now;
    
    char strftime_buf[64];
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "Time Synced Successfully via Wi-Fi: %s", strftime_buf);
    
    return true;
}

void time_get_timestamp_str(char *buf, size_t size) {
    time_t now;
    time(&now);
    snprintf(buf, size, "%llu", (unsigned long long)now);
}
