#ifndef DATA_REPORTER_H
#define DATA_REPORTER_H

#include <stdbool.h>

// Send the "Device Ready" Packet (Call on boot)
bool data_reporter_send_ready(void);

// Send "Status" Packet (Call every 15 mins)
// Message: "no issue"
bool data_reporter_send_status(void);

// NEW: Send Emergency Packet (Called by Gas Sensor)
// Message: "Gas Leak Detected"
bool data_reporter_send_emergency(int gas_value);

#endif