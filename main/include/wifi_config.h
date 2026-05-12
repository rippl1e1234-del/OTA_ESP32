#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <stdbool.h>

// Initialize Wi-Fi and NVS
void wifi_init_and_check(void);

// Start the SoftAP manually (for your button logic later)
void start_ap_provisioning_mode(void);

// Check if we have SSID/Pass saved
bool is_wifi_credentials_saved(void);

#endif
