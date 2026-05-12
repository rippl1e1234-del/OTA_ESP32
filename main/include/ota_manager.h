#ifndef OTA_MANAGER_H
#define OTA_MANAGER_H

#include <stdbool.h>

/**
 * @brief Start MQTT client for OTA firmware delivery.
 * 
 * @param broker_uri   MQTT broker URI, e.g., "mqtts://your-broker:8883"
 * @param ca_cert_pem  PEM string of CA certificate, or NULL to use ESP bundle
 * @return true if client started successfully, false otherwise
 */
bool ota_init_mqtt(const char *broker_uri, const char *ca_cert_pem);

/**
 * @brief Stop the MQTT client (optional, e.g., when entering deep sleep)
 */
void ota_deinit_mqtt(void);

bool ota_perform_gsm_https_update(const char *version_check_url);
#endif
