#ifndef GSM_MANAGER_H
#define GSM_MANAGER_H

#include <stdbool.h>
#include <string.h>
#include "esp_ota_ops.h"

// --- PINS ---
#define GSM_TX_PIN      17
#define GSM_RX_PIN      16
#define GSM_PWRKEY_PIN  23
#define GSM_LED_PIN     27      
#define GSM_BAUD_RATE   115200
#define GSM_UART_NUM    UART_NUM_1
#define GSM_BUF_SIZE    1024

// --- FUNCTIONS ---
void gsm_init(void);
void gsm_power_on(void);
void gsm_start_monitoring(void); 

// Send JSON Data via HTTP/HTTPS POST
// Returns true if HTTP Status Code is 200
bool gsm_http_post(const char *url, const char *json_data);
bool gsm_get_time_str(char *buf, size_t buf_size);
// Getter for status
bool gsm_is_connected(void);

// --- NEW FUNCTION: EMERGENCY ABORT ---
// Call this to stop any ongoing GSM operation immediately
void gsm_abort_current_operation(void);
bool gsm_ota_download_to_partition(const char *url, esp_ota_handle_t ota_handle, size_t *out_size);
bool gsm_http_get_json(const char *url, char *response, size_t size);
bool gsm_is_ota_busy(void);

#endif