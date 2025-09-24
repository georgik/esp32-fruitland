/**
 * @file accelerometer.c
 * @brief Accelerometer input implementation for ESP32-Fruitland
 * 
 * Based on ICM42670 6-axis accelerometer for ESP32-S3-BOX-3.
 * Provides motion control by translating device tilt to directional input.
 */

#include "accelerometer.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "accelerometer";

#ifdef CONFIG_FRUITLAND_ACCELEROMETER_INPUT

#include "driver/i2c_master.h"
#include "icm42670.h"
#include "bsp/esp-box-3.h"
#include "SDL_internal.h"
#include "events/SDL_keyboard_c.h"

// Accelerometer state
static bool accelerometer_initialized = false;
static icm42670_handle_t icm42670_handle = NULL;
static i2c_master_bus_handle_t i2c_handle = NULL;

// Configuration parameters
static float tilt_threshold = 0.3f; // G-force threshold to trigger movement
static float deadzone = 0.1f; // Deadzone to prevent jitter
static bool invert_x = true; // Invert X-axis - ESP32-S3-BOX-3 needs this for correct tilt direction
static bool invert_y = false; // Invert Y-axis if needed

// Previous key states to avoid duplicate events
static bool prev_left = false;
static bool prev_right = false;
static bool prev_up = false;
static bool prev_down = false;

/**
 * @brief Send SDL keyboard event for accelerometer input
 */
static void send_accel_key_event(SDL_Scancode scancode, bool pressed) {
    // Get the keyboard ID (assuming there's only one for simplicity)
    int num_keyboards;
    SDL_KeyboardID *keyboard_ids = SDL_GetKeyboards(&num_keyboards);

    if (num_keyboards == 0) {
        ESP_LOGD(TAG, "No SDL keyboards registered, adding virtual keyboard");
        SDL_AddKeyboard(2, "ESP32 Accelerometer");
        keyboard_ids = SDL_GetKeyboards(&num_keyboards);
        if (num_keyboards == 0) {
            ESP_LOGE(TAG, "Failed to add virtual keyboard");
            return;
        }
    }

    SDL_KeyboardID keyboardID = keyboard_ids[0];

    ESP_LOGD(TAG, "Accelerometer key %s: scancode=%d",
             pressed ? "pressed" : "released", scancode);

    // Send key event to SDL (use a special key_id for accelerometer)
    SDL_SendKeyboardKey(SDL_GetTicks(), keyboardID, 100 + scancode, scancode, pressed);
}

/**
 * @brief Process accelerometer readings and generate key events
 */
static void process_accelerometer_data(const icm42670_value_t *accel_data) {
    float x = accel_data->x;
    float y = accel_data->y;

    // Apply axis inversions if configured
    if (invert_x) x = -x;
    if (invert_y) y = -y;

    // Calculate current key states based on tilt
    bool current_left = (x < -tilt_threshold);
    bool current_right = (x > tilt_threshold);
    bool current_up = (y > tilt_threshold); // Assuming positive Y is up
    bool current_down = (y < -tilt_threshold); // Assuming negative Y is down

    // Apply deadzone - if we're within deadzone of center, no movement
    if (fabs(x) < deadzone) {
        current_left = false;
        current_right = false;
    }
    if (fabs(y) < deadzone) {
        current_up = false;
        current_down = false;
    }

    // Send key press/release events only when state changes
    if (current_left != prev_left) {
        send_accel_key_event(SDL_SCANCODE_LEFT, current_left);
        prev_left = current_left;
    }

    if (current_right != prev_right) {
        send_accel_key_event(SDL_SCANCODE_RIGHT, current_right);
        prev_right = current_right;
    }

    if (current_up != prev_up) {
        send_accel_key_event(SDL_SCANCODE_UP, current_up);
        prev_up = current_up;
    }

    if (current_down != prev_down) {
        send_accel_key_event(SDL_SCANCODE_DOWN, current_down);
        prev_down = current_down;
    }

    ESP_LOGV(TAG, "Accel: x=%.2f, y=%.2f -> L:%d R:%d U:%d D:%d",
             x, y, current_left, current_right, current_up, current_down);
}

// Public API implementation

esp_err_t init_accelerometer(void) {
    if (accelerometer_initialized) {
        ESP_LOGW(TAG, "Accelerometer already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing accelerometer input");

    // Initialize I2C bus using BSP
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // Get I2C handle from BSP
    i2c_handle = bsp_i2c_get_handle();
    if (!i2c_handle) {
        ESP_LOGE(TAG, "Failed to get I2C handle from BSP");
        return ESP_FAIL;
    }

    // Create ICM42670 sensor handle
    ret = icm42670_create(i2c_handle, ICM42670_I2C_ADDRESS, &icm42670_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ICM42670 handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure accelerometer for gaming
    const icm42670_cfg_t imu_cfg = {
        .acce_fs = ACCE_FS_2G, // ±2g range for sensitive tilt detection
        .acce_odr = ACCE_ODR_100HZ, // 100Hz for smooth gaming (was 400Hz but that might be too fast)
        .gyro_fs = GYRO_FS_2000DPS, // We don't use gyro but need to configure it
        .gyro_odr = GYRO_ODR_100HZ, // Match accelerometer ODR
    };

    ret = icm42670_config(icm42670_handle, &imu_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure ICM42670: %s", esp_err_to_name(ret));
        icm42670_delete(icm42670_handle);
        icm42670_handle = NULL;
        return ret;
    }

    // Enable accelerometer
    ret = icm42670_acce_set_pwr(icm42670_handle, ACCE_PWR_LOWNOISE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable accelerometer: %s", esp_err_to_name(ret));
        icm42670_delete(icm42670_handle);
        icm42670_handle = NULL;
        return ret;
    }

    // Test sensor communication
    uint8_t device_id;
    ret = icm42670_get_deviceid(icm42670_handle, &device_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read device ID: %s", esp_err_to_name(ret));
        icm42670_delete(icm42670_handle);
        icm42670_handle = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "ICM42670 device ID: 0x%02x", device_id);

    // Add virtual SDL keyboard for accelerometer
    SDL_AddKeyboard(2, "ESP32 Accelerometer");

    accelerometer_initialized = true;
    ESP_LOGI(TAG, "Accelerometer input initialized successfully");
    return ESP_OK;
}

void process_accelerometer(void) {
    if (!accelerometer_initialized || !icm42670_handle) {
        return;
    }

    // Read accelerometer data
    icm42670_value_t accel_data;
    esp_err_t ret = icm42670_get_acce_value(icm42670_handle, &accel_data);

    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Failed to read accelerometer data: %s", esp_err_to_name(ret));
        return;
    }

    // Process the data and generate key events
    process_accelerometer_data(&accel_data);
}

bool is_accelerometer_available(void) {
    return accelerometer_initialized;
}

void cleanup_accelerometer(void) {
    if (!accelerometer_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Cleaning up accelerometer resources");

    // Release all pressed keys
    if (prev_left) send_accel_key_event(SDL_SCANCODE_LEFT, false);
    if (prev_right) send_accel_key_event(SDL_SCANCODE_RIGHT, false);
    if (prev_up) send_accel_key_event(SDL_SCANCODE_UP, false);
    if (prev_down) send_accel_key_event(SDL_SCANCODE_DOWN, false);

    prev_left = prev_right = prev_up = prev_down = false;

    if (icm42670_handle) {
        icm42670_delete(icm42670_handle);
        icm42670_handle = NULL;
    }

    // Note: We don't deinitialize I2C bus as it might be used by other components
    // The BSP manages the I2C bus lifecycle

    accelerometer_initialized = false;
    ESP_LOGI(TAG, "Accelerometer cleanup completed");
}

void set_accelerometer_threshold(float threshold) {
    if (threshold >= 0.1f && threshold <= 1.0f) {
        tilt_threshold = threshold;
        ESP_LOGI(TAG, "Accelerometer threshold set to %.2f g", threshold);
    } else {
        ESP_LOGW(TAG, "Invalid threshold %.2f, must be between 0.1 and 1.0", threshold);
    }
}

void set_accelerometer_deadzone(float deadzone_val) {
    if (deadzone_val >= 0.05f && deadzone_val <= 0.5f) {
        deadzone = deadzone_val;
        ESP_LOGI(TAG, "Accelerometer deadzone set to %.2f g", deadzone_val);
    } else {
        ESP_LOGW(TAG, "Invalid deadzone %.2f, must be between 0.05 and 0.5", deadzone_val);
    }
}

#else // !CONFIG_FRUITLAND_ACCELEROMETER_INPUT

// Stub implementations for when accelerometer support is disabled
esp_err_t init_accelerometer(void) {
    ESP_LOGW(TAG, "Accelerometer input is disabled in configuration");
    return ESP_ERR_NOT_SUPPORTED;
}

void process_accelerometer(void) {
    // No-op when accelerometer support is disabled
}

bool is_accelerometer_available(void) {
    return false;
}

void cleanup_accelerometer(void) {
    // No-op when accelerometer support is disabled
}

void set_accelerometer_threshold(float threshold) {
    ESP_LOGW(TAG, "Accelerometer input is disabled in configuration");
}

void set_accelerometer_deadzone(float deadzone_val) {
    ESP_LOGW(TAG, "Accelerometer input is disabled in configuration");
}

#endif // CONFIG_FRUITLAND_ACCELEROMETER_INPUT
