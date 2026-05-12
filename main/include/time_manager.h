#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <stdbool.h>
#include <stddef.h>

// Init SNTP (Call once on boot)
void time_manager_init(void);

// Try to sync time using available networks (Wi-Fi priority)
// Returns true if time is valid/synced
bool time_sync(void);

// Get current Unix timestamp as a string (e.g., "1769241696")
void time_get_timestamp_str(char *buf, size_t buf_size);
bool ping_google(void); // NEW: Ping Google to check internet connectivity
bool wifi_is_connected(void); // NEW: Check Wi-Fi connection status

#endif