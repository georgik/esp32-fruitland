/**
 * @file keyboard.h
 * @brief USB HID Keyboard support for ESP32-Fruitland
 * 
 * This module provides USB keyboard input handling for ESP32-P4 boards,
 * translating HID events to SDL keyboard events for game controls.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "SDL3/SDL.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_FRUITLAND_USB_KEYBOARD_SUPPORT

/**
 * @brief Initialize USB HID keyboard support
 * 
 * Sets up USB host stack and HID keyboard handling.
 * Only available on ESP32-P4 targets with USB host support.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t init_keyboard(void);

/**
 * @brief Process keyboard events
 * 
 * Should be called periodically to handle USB HID events
 * and convert them to SDL keyboard events.
 */
void process_keyboard(void);

/**
 * @brief Check if keyboard is initialized and available
 * 
 * @return true if keyboard is available, false otherwise
 */
bool is_keyboard_available(void);

/**
 * @brief Cleanup keyboard resources
 * 
 * Stops USB host stack and frees resources
 */
void cleanup_keyboard(void);

#endif // CONFIG_FRUITLAND_USB_KEYBOARD_SUPPORT

#ifdef __cplusplus
}
#endif
