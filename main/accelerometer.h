/**
 * @file accelerometer.h
 * @brief Accelerometer-based input support for ESP32-Fruitland
 * 
 * This module provides accelerometer motion control handling for ESP32-S3-BOX-3,
 * translating device tilt to SDL keyboard events for game controls.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "SDL3/SDL.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {

#endif

#ifdef CONFIG_FRUITLAND_ACCELEROMETER_INPUT

/**
 * @brief Initialize accelerometer input support
 * 
 * Sets up I2C bus and ICM42670 accelerometer sensor.
 * Only available on boards with accelerometer support (e.g., ESP32-S3-BOX-3).
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t init_accelerometer(void);

/**
 * @brief Process accelerometer input
 * 
 * Should be called periodically to read accelerometer data
 * and convert tilt movements to SDL keyboard events.
 */
void process_accelerometer(void);

/**
 * @brief Check if accelerometer is initialized and available
 * 
 * @return true if accelerometer is available, false otherwise
 */
bool is_accelerometer_available(void);

/**
 * @brief Cleanup accelerometer resources
 * 
 * Stops I2C communication and frees resources
 */
void cleanup_accelerometer(void);

/**
 * @brief Set accelerometer sensitivity threshold
 * 
 * @param threshold Tilt threshold in g-force (0.1 to 1.0, default 0.3)
 */
void set_accelerometer_threshold(float threshold);

/**
 * @brief Set accelerometer deadzone
 * 
 * @param deadzone Deadzone in g-force to prevent jitter (0.05 to 0.5, default 0.1)
 */
void set_accelerometer_deadzone(float deadzone);

#endif // CONFIG_FRUITLAND_ACCELEROMETER_INPUT

#ifdef __cplusplus
}
#endif
