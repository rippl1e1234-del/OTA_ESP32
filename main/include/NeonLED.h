#ifndef NEON_LED_H
#define NEON_LED_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void neon_init(void);
void neon_set_rgb(uint8_t r, uint8_t g, uint8_t b);

typedef enum {
    NEON_OFF,
    NEON_BOOT,        // White (Boot sequence)
    NEON_AP_MODE,     // Magenta (Config Mode)
    NEON_CONNECTING,  // Blue (Trying to connect)
    NEON_STABLE,      // Green (Normal/Idle - Auto OFF after 3 min)
    NEON_FAIL,        // Red (Gas Leak)
    NEON_COOLDOWN     // Yellow (Sensor Cooldown)
} neon_status_t;

void neon_set_status(neon_status_t status);

#ifdef __cplusplus
}
#endif

#endif