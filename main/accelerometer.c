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
#include "esp_timer.h"

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

// Configuration parameters for intuitive tilt detection - tuned for ESP32-S3-BOX-3
static float small_tilt_threshold = 0.2f;   // Light tilt threshold for single move (reduced for easier use)
static float large_tilt_threshold = 0.45f;  // Stronger tilt threshold for continuous movement (reduced)
static float deadzone = 0.08f;              // Deadzone to prevent jitter (reduced for more sensitivity)
static bool invert_x = true;                // Invert X-axis - ESP32-S3-BOX-3 needs this for correct tilt direction
static bool invert_y = false;               // Invert Y-axis if needed

// Enhanced state tracking for improved tilt detection
static bool prev_left = false;
static bool prev_right = false;
static bool prev_up = false;
static bool prev_down = false;

// Direct movement triggering system - bypasses SDL keyboard events for single moves
static uint64_t last_move_time[4] = {0};               // LEFT, RIGHT, UP, DOWN
static bool tilt_gesture_active[4] = {false};          // Track if we're in the middle of a tilt gesture
static uint64_t deadzone_enter_time = 0;               // When we entered deadzone
static bool in_deadzone = false;                       // Are we currently in deadzone
static const uint64_t move_cooldown_us = 200000;       // 200ms cooldown between single moves
static const uint64_t gesture_reset_time_us = 100000;  // 100ms in deadzone before allowing new gesture

// Direct movement interface - will be called by main game
static int pending_single_move = 0;  // 0=none, 1=UP, 2=DOWN, 3=LEFT, 4=RIGHT

// Direction indices for arrays
#define DIR_LEFT 0
#define DIR_RIGHT 1
#define DIR_UP 2
#define DIR_DOWN 3

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

    ESP_LOGD(TAG, "Accelerometer key %s: scancode=%d", pressed ? "pressed" : "released", scancode);

    // Send key event to SDL (use a special key_id for accelerometer)
    SDL_SendKeyboardKey(SDL_GetTicks(), keyboardID, 100 + scancode, scancode, pressed);
}

/**
 * @brief Get current time in microseconds for timing calculations
 */
static uint64_t get_time_us() {
    return esp_timer_get_time();
}

/**
 * @brief Handle single move with direct triggering - bypasses SDL for precision
 */
static void handle_single_move(int dir_index, uint64_t current_time) {
    // ONE-SHOT LOGIC: Only trigger if this is a new gesture and no pending move
    if (!tilt_gesture_active[dir_index] && pending_single_move == 0 &&
        (current_time - last_move_time[dir_index]) >= move_cooldown_us) {

        // Mark this gesture as active to prevent repeat
        tilt_gesture_active[dir_index] = true;

        // Queue the single move (1=UP, 2=DOWN, 3=LEFT, 4=RIGHT)
        pending_single_move = dir_index + 1;
        last_move_time[dir_index] = current_time;

        const char *dir_names[] = {"LEFT", "RIGHT", "UP", "DOWN"};
        ESP_LOGI(TAG, "🎮 %s single move queued - precise one tile", dir_names[dir_index]);
    }
}

/**
 * @brief Check if there's a pending single move from accelerometer
 * @return Movement direction: 0=none, 1=UP, 2=DOWN, 3=LEFT, 4=RIGHT
 */
int accelerometer_get_pending_move(void) {
    return pending_single_move;
}

/**
 * @brief Consume the pending single move (call after processing it)
 */
void accelerometer_consume_pending_move(void) {
    if (pending_single_move != 0) {
        ESP_LOGI(TAG, "✅ Single move consumed");
        pending_single_move = 0;
    }
}

/**
 * @brief Process accelerometer readings with improved tilt detection algorithm
 *
 * This function implements a dual-threshold system:
 * - Small tilts generate single keystrokes (good for precise navigation)
 * - Large tilts switch to continuous movement mode (good for fast movement)
 */
static void process_accelerometer_data(const icm42670_value_t *accel_data) {
    float x = accel_data->x;
    float y = accel_data->y;
    uint64_t current_time = get_time_us();

    // Apply axis inversions if configured
    if (invert_x) x = -x;
    if (invert_y) y = -y;

    // Apply deadzone first - if we're within deadzone, reset everything
    if (fabs(x) < deadzone && fabs(y) < deadzone) {
        // Track how long we've been in deadzone
        if (!in_deadzone) {
            in_deadzone = true;
            deadzone_enter_time = current_time;
        }

        // After being in deadzone long enough, reset gesture states
        if (current_time - deadzone_enter_time >= gesture_reset_time_us) {
            for (int i = 0; i < 4; i++) {
                tilt_gesture_active[i] = false;  // Reset gesture tracking
            }
        }

        // Release any currently pressed continuous keys
        if (prev_left) {
            send_accel_key_event(SDL_SCANCODE_LEFT, false);
            prev_left = false;
        }
        if (prev_right) {
            send_accel_key_event(SDL_SCANCODE_RIGHT, false);
            prev_right = false;
        }
        if (prev_up) {
            send_accel_key_event(SDL_SCANCODE_UP, false);
            prev_up = false;
        }
        if (prev_down) {
            send_accel_key_event(SDL_SCANCODE_DOWN, false);
            prev_down = false;
        }
        return;
    } else {
        // We're no longer in deadzone
        in_deadzone = false;
    }

    // Process each direction with the improved algorithm

    // LEFT movement (negative X)
    if (x < -deadzone) {
        float tilt_magnitude = fabs(x);

        if (tilt_magnitude >= large_tilt_threshold) {
            // Large tilt - continuous movement mode
            if (!prev_left) {
                send_accel_key_event(SDL_SCANCODE_LEFT, true);
                prev_left = true;
                ESP_LOGI(TAG, "⬅️ LEFT continuous mode (tilt=%.2f) - held key", tilt_magnitude);
            }
        } else if (tilt_magnitude >= small_tilt_threshold) {
            // Small tilt - single move mode
            handle_single_move(DIR_LEFT, current_time);
        }
    } else {
        // Not tilting left - release if was pressed
        if (prev_left) {
            send_accel_key_event(SDL_SCANCODE_LEFT, false);
            prev_left = false;
        }
        // Don't immediately reset keystroke_sent - let deadzone timeout handle it
    }

    // RIGHT movement (positive X)
    if (x > deadzone) {
        float tilt_magnitude = fabs(x);

        if (tilt_magnitude >= large_tilt_threshold) {
            // Large tilt - continuous movement mode
            if (!prev_right) {
                send_accel_key_event(SDL_SCANCODE_RIGHT, true);
                prev_right = true;
                ESP_LOGI(TAG, "➡️ RIGHT continuous mode (tilt=%.2f) - held key", tilt_magnitude);
            }
        } else if (tilt_magnitude >= small_tilt_threshold) {
            // Small tilt - single move mode
            handle_single_move(DIR_RIGHT, current_time);
        }
    } else {
        // Not tilting right - release if was pressed
        if (prev_right) {
            send_accel_key_event(SDL_SCANCODE_RIGHT, false);
            prev_right = false;
        }
        // Don't immediately reset keystroke_sent - let deadzone timeout handle it
    }

    // UP movement (positive Y)
    if (y > deadzone) {
        float tilt_magnitude = fabs(y);

        if (tilt_magnitude >= large_tilt_threshold) {
            // Large tilt - continuous movement mode
            if (!prev_up) {
                send_accel_key_event(SDL_SCANCODE_UP, true);
                prev_up = true;
                ESP_LOGI(TAG, "⬆️ UP continuous mode (tilt=%.2f) - held key", tilt_magnitude);
            }
        } else if (tilt_magnitude >= small_tilt_threshold) {
            // Small tilt - single move mode
            handle_single_move(DIR_UP, current_time);
        }
    } else {
        // Not tilting up - release if was pressed
        if (prev_up) {
            send_accel_key_event(SDL_SCANCODE_UP, false);
            prev_up = false;
        }
        // Don't immediately reset keystroke_sent - let deadzone timeout handle it
    }

    // DOWN movement (negative Y)
    if (y < -deadzone) {
        float tilt_magnitude = fabs(y);

        if (tilt_magnitude >= large_tilt_threshold) {
            // Large tilt - continuous movement mode
            if (!prev_down) {
                send_accel_key_event(SDL_SCANCODE_DOWN, true);
                prev_down = true;
                ESP_LOGI(TAG, "⬇️ DOWN continuous mode (tilt=%.2f) - held key", tilt_magnitude);
            }
        } else if (tilt_magnitude >= small_tilt_threshold) {
            // Small tilt - single move mode
            handle_single_move(DIR_DOWN, current_time);
        }
    } else {
        // Not tilting down - release if was pressed
        if (prev_down) {
            send_accel_key_event(SDL_SCANCODE_DOWN, false);
            prev_down = false;
        }
        // Don't immediately reset keystroke_sent - let deadzone timeout handle it
    }

    ESP_LOGV(TAG, "Accel: x=%.2f, y=%.2f -> L:%d R:%d U:%d D:%d (continuous mode)", x, y, prev_left, prev_right,
             prev_up, prev_down);
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
        .acce_fs = ACCE_FS_2G,       // ±2g range for sensitive tilt detection
        .acce_odr = ACCE_ODR_100HZ,  // 100Hz for smooth gaming (was 400Hz but that might be too fast)
        .gyro_fs = GYRO_FS_2000DPS,  // We don't use gyro but need to configure it
        .gyro_odr = GYRO_ODR_100HZ,  // Match accelerometer ODR
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
    if (!accelerometer_initialized || !icm42670_handle) { return; }

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
    if (!accelerometer_initialized) { return; }

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
    // For backward compatibility, set both thresholds based on single value
    if (threshold >= 0.1f && threshold <= 1.0f) {
        small_tilt_threshold = threshold;
        large_tilt_threshold = threshold * 1.8f;                       // 180% of threshold for large tilt
        if (large_tilt_threshold > 1.0f) large_tilt_threshold = 1.0f;  // Cap at 1g
        ESP_LOGI(TAG, "Accelerometer thresholds set: small=%.2f g, large=%.2f g", small_tilt_threshold,
                 large_tilt_threshold);
    } else {
        ESP_LOGW(TAG, "Invalid threshold %.2f, must be between 0.1 and 1.0", threshold);
    }
}

void set_accelerometer_thresholds(float small_threshold, float large_threshold) {
    if (small_threshold >= 0.1f && small_threshold <= 0.8f && large_threshold >= 0.2f && large_threshold <= 1.0f &&
        large_threshold > small_threshold) {

        small_tilt_threshold = small_threshold;
        large_tilt_threshold = large_threshold;
        ESP_LOGI(TAG, "Accelerometer thresholds set: small=%.2f g, large=%.2f g", small_tilt_threshold,
                 large_tilt_threshold);
    } else {
        ESP_LOGW(TAG,
                 "Invalid thresholds small=%.2f, large=%.2f. Requirements: 0.1 <= small <= 0.8, 0.2 <= large <= 1.0, "
                 "large > small",
                 small_threshold, large_threshold);
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

#else  // !CONFIG_FRUITLAND_ACCELEROMETER_INPUT

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

void set_accelerometer_thresholds(float small_threshold, float large_threshold) {
    ESP_LOGW(TAG, "Accelerometer input is disabled in configuration");
}

void set_accelerometer_deadzone(float deadzone_val) {
    ESP_LOGW(TAG, "Accelerometer input is disabled in configuration");
}

#endif  // CONFIG_FRUITLAND_ACCELEROMETER_INPUT
