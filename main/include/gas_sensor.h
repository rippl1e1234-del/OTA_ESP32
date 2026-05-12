#ifndef GAS_SENSOR_H
#define GAS_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

// --- HARDWARE CONFIGURATION ---
#define GAS_ADC_CHANNEL     ADC_CHANNEL_6   
#define BUZZER_GPIO         GPIO_NUM_32     
#define GAS_THRESHOLD_DEFAULT 750          

// --- TIMING CONFIGURATION (Shared) ---
// 48 Hours in Minutes = 2880
#define PRE_HEAT_DURATION_MIN   3   // 2880 
#define WARM_UP_DURATION_MS     (3 * 60 * 1000) // 3 Minutes

// --- PUBLIC FUNCTIONS ---
void gas_sensor_init(void);
bool gas_is_emergency(void);
// NEW: Silences the buzzer during an alarm
void gas_silence_buzzer(void);

#endif