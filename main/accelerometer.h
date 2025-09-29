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
 * @brief Set accelerometer sensitivity threshold (backward compatibility)
 * 
 * @param threshold Tilt threshold in g-force (0.1 to 1.0, default 0.3)
 *                  This automatically sets small and large thresholds based on this value
 */
void set_accelerometer_threshold(float threshold);

/**
 * @brief Set separate thresholds for improved tilt detection
 * 
 * This allows fine-tuning of the dual-threshold tilt detection system:
 * - Small tilts generate single keystrokes (good for precise navigation through gaps)
 * - Large tilts enable continuous movement mode (good for fast traversal)
 * 
 * @param small_threshold Small tilt threshold for single keystrokes (0.1 to 0.8g, default 0.2g)
 * @param large_threshold Large tilt threshold for continuous movement (0.2 to 1.0g, default 0.45g)
 */
void set_accelerometer_thresholds(float small_threshold, float large_threshold);

/**
 * @brief Set accelerometer deadzone
 * 
 * @param deadzone Deadzone in g-force to prevent jitter (0.05 to 0.5, default 0.1)
 */
void set_accelerometer_deadzone(float deadzone);

/**
 * @brief Check if there's a pending single move from accelerometer
 * 
 * This function allows the game to check for precise single-tile movements
 * triggered by small accelerometer tilts, bypassing SDL keyboard events.
 * 
 * @return Movement direction: 0=none, 1=UP, 2=DOWN, 3=LEFT, 4=RIGHT
 */
int accelerometer_get_pending_move(void);

/**
 * @brief Consume the pending single move (call after processing it)
 * 
 * This function should be called after the game has processed the pending move
 * to clear it and allow new moves to be queued.
 */
void accelerometer_consume_pending_move(void);

#endif // CONFIG_FRUITLAND_ACCELEROMETER_INPUT

#ifdef __cplusplus
}
#endif
