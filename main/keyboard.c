/**
 * @file keyboard.c
 * @brief USB HID Keyboard implementation for ESP32-Fruitland
 * 
 * Based on OpenTyrian's keyboard implementation, adapted for ESP32-Fruitland.
 * Provides USB HID keyboard support for ESP32-P4 boards.
 */

#include "keyboard.h"
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"

#ifdef CONFIG_IDF_TARGET_ESP32P4

#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "SDL_internal.h"
#include "events/SDL_keyboard_c.h"

static const char *TAG = "keyboard";

// USB and HID event handling
static QueueHandle_t app_event_queue = NULL;
static sem_t usb_task_semaphore;
static pthread_t usb_event_thread;
static bool keyboard_initialized = false;

#define APP_QUIT_PIN GPIO_NUM_0

// Event queue structure
typedef enum {
    APP_EVENT = 0,
    APP_EVENT_HID_HOST
} app_event_group_t;

typedef struct {
    app_event_group_t event_group;
    struct {
        hid_host_device_handle_t handle;
        hid_host_driver_event_t event;
        void *arg;
    } hid_host_device;
} app_event_queue_t;

// Key event structure
typedef struct {
    enum key_state {
        KEY_STATE_PRESSED = 0x00,
        KEY_STATE_RELEASED = 0x01
    } state;
    uint8_t modifier;
    uint8_t key_code;
} key_event_t;

// Protocol names for logging
static const char *hid_proto_name_str[] = {
    "NONE",
    "KEYBOARD",
    "MOUSE"
};

/**
 * @brief Convert HID key code to SDL scancode
 */
static SDL_Scancode convert_hid_to_sdl_scancode(uint8_t hid_code) {
    switch (hid_code) {
        // Alphabet (A-Z)
        case HID_KEY_A: return SDL_SCANCODE_A;
        case HID_KEY_B: return SDL_SCANCODE_B;
        case HID_KEY_C: return SDL_SCANCODE_C;
        case HID_KEY_D: return SDL_SCANCODE_D;
        case HID_KEY_E: return SDL_SCANCODE_E;
        case HID_KEY_F: return SDL_SCANCODE_F;
        case HID_KEY_G: return SDL_SCANCODE_G;
        case HID_KEY_H: return SDL_SCANCODE_H;
        case HID_KEY_I: return SDL_SCANCODE_I;
        case HID_KEY_J: return SDL_SCANCODE_J;
        case HID_KEY_K: return SDL_SCANCODE_K;
        case HID_KEY_L: return SDL_SCANCODE_L;
        case HID_KEY_M: return SDL_SCANCODE_M;
        case HID_KEY_N: return SDL_SCANCODE_N;
        case HID_KEY_O: return SDL_SCANCODE_O;
        case HID_KEY_P: return SDL_SCANCODE_P;
        case HID_KEY_Q: return SDL_SCANCODE_Q;
        case HID_KEY_R: return SDL_SCANCODE_R;
        case HID_KEY_S: return SDL_SCANCODE_S;
        case HID_KEY_T: return SDL_SCANCODE_T;
        case HID_KEY_U: return SDL_SCANCODE_U;
        case HID_KEY_V: return SDL_SCANCODE_V;
        case HID_KEY_W: return SDL_SCANCODE_W;
        case HID_KEY_X: return SDL_SCANCODE_X;
        case HID_KEY_Y: return SDL_SCANCODE_Y;
        case HID_KEY_Z: return SDL_SCANCODE_Z;

        // Numbers (0-9)
        case HID_KEY_1: return SDL_SCANCODE_1;
        case HID_KEY_2: return SDL_SCANCODE_2;
        case HID_KEY_3: return SDL_SCANCODE_3;
        case HID_KEY_4: return SDL_SCANCODE_4;
        case HID_KEY_5: return SDL_SCANCODE_5;
        case HID_KEY_6: return SDL_SCANCODE_6;
        case HID_KEY_7: return SDL_SCANCODE_7;
        case HID_KEY_8: return SDL_SCANCODE_8;
        case HID_KEY_9: return SDL_SCANCODE_9;
        case HID_KEY_0: return SDL_SCANCODE_0;

        // Special keys
        case HID_KEY_ENTER: return SDL_SCANCODE_RETURN;
        case HID_KEY_ESC: return SDL_SCANCODE_ESCAPE;
        case HID_KEY_SPACE: return SDL_SCANCODE_SPACE;
        case HID_KEY_DEL: return SDL_SCANCODE_BACKSPACE;
        case HID_KEY_TAB: return SDL_SCANCODE_TAB;

        // Arrow keys
        case HID_KEY_UP: return SDL_SCANCODE_UP;
        case HID_KEY_DOWN: return SDL_SCANCODE_DOWN;
        case HID_KEY_LEFT: return SDL_SCANCODE_LEFT;
        case HID_KEY_RIGHT: return SDL_SCANCODE_RIGHT;

        // Function keys (F1-F12)
        case HID_KEY_F1: return SDL_SCANCODE_F1;
        case HID_KEY_F2: return SDL_SCANCODE_F2;
        case HID_KEY_F3: return SDL_SCANCODE_F3;
        case HID_KEY_F4: return SDL_SCANCODE_F4;
        case HID_KEY_F5: return SDL_SCANCODE_F5;
        case HID_KEY_F6: return SDL_SCANCODE_F6;
        case HID_KEY_F7: return SDL_SCANCODE_F7;
        case HID_KEY_F8: return SDL_SCANCODE_F8;
        case HID_KEY_F9: return SDL_SCANCODE_F9;
        case HID_KEY_F10: return SDL_SCANCODE_F10;
        case HID_KEY_F11: return SDL_SCANCODE_F11;
        case HID_KEY_F12: return SDL_SCANCODE_F12;

        // Modifier keys (note: right keys have same codes as left in this HID implementation)
        case HID_KEY_LEFT_CONTROL: return SDL_SCANCODE_LCTRL;
        case HID_KEY_LEFT_SHIFT: return SDL_SCANCODE_LSHIFT;
        case HID_KEY_LEFT_ALT: return SDL_SCANCODE_LALT;
        // Right keys are handled via modifier byte, not key codes

        default: return SDL_SCANCODE_UNKNOWN;
    }
}

/**
 * @brief Handle key events and send them to SDL
 */
static void key_event_callback(key_event_t *key_event) {
    // Get the keyboard ID (assuming there's only one for simplicity)
    int num_keyboards;
    SDL_KeyboardID *keyboard_ids = SDL_GetKeyboards(&num_keyboards);

    if (num_keyboards == 0) {
        ESP_LOGW(TAG, "No SDL keyboards registered, adding virtual keyboard");
        SDL_AddKeyboard(1, "ESP32 USB Keyboard");
        keyboard_ids = SDL_GetKeyboards(&num_keyboards);
        if (num_keyboards == 0) {
            ESP_LOGE(TAG, "Failed to add virtual keyboard");
            return;
        }
    }

    SDL_KeyboardID keyboardID = keyboard_ids[0];
    SDL_Scancode scancode = convert_hid_to_sdl_scancode(key_event->key_code);
    
    if (scancode != SDL_SCANCODE_UNKNOWN) {
        bool pressed = (key_event->state == KEY_STATE_PRESSED);
        ESP_LOGD(TAG, "Key %s: HID=0x%02x SDL=%d", 
                pressed ? "pressed" : "released", key_event->key_code, scancode);
        
        // Send key event to SDL
        SDL_SendKeyboardKey(SDL_GetTicks(), keyboardID, key_event->key_code, scancode, pressed);
    }
}

/**
 * @brief Check if key is found in array
 */
static inline bool key_found(const uint8_t *const src, uint8_t key, unsigned int length) {
    for (unsigned int i = 0; i < length; i++) {
        if (src[i] == key) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Handle keyboard report from HID
 */
static void hid_host_keyboard_report_callback(const uint8_t *const data, const int length) {
    hid_keyboard_input_report_boot_t *kb_report = (hid_keyboard_input_report_boot_t *)data;

    if (length < sizeof(hid_keyboard_input_report_boot_t)) {
        return;
    }

    static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX] = { 0 };
    key_event_t key_event;

    for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
        // Key has been released
        if (prev_keys[i] > HID_KEY_ERROR_UNDEFINED &&
                !key_found(kb_report->key, prev_keys[i], HID_KEYBOARD_KEY_MAX)) {
            key_event.key_code = prev_keys[i];
            key_event.modifier = 0;
            key_event.state = KEY_STATE_RELEASED;
            key_event_callback(&key_event);
        }

        // Key has been pressed
        if (kb_report->key[i] > HID_KEY_ERROR_UNDEFINED &&
                !key_found(prev_keys, kb_report->key[i], HID_KEYBOARD_KEY_MAX)) {
            key_event.key_code = kb_report->key[i];
            key_event.modifier = kb_report->modifier.val;
            key_event.state = KEY_STATE_PRESSED;
            key_event_callback(&key_event);
        }
    }

    memcpy(prev_keys, &kb_report->key, HID_KEYBOARD_KEY_MAX);
}

/**
 * @brief HID host interface callback
 */
static void hid_host_interface_callback(hid_host_device_handle_t hid_device_handle,
                                      const hid_host_interface_event_t event,
                                      void *arg) {
    uint8_t data[64] = { 0 };
    size_t data_length = 0;
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
        ESP_ERROR_CHECK(hid_host_device_get_raw_input_report_data(hid_device_handle,
                                                                  data,
                                                                  64,
                                                                  &data_length));

        if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class &&
            HID_PROTOCOL_KEYBOARD == dev_params.proto) {
            hid_host_keyboard_report_callback(data, data_length);
        }
        break;
        
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HID Device, protocol '%s' DISCONNECTED",
                 hid_proto_name_str[dev_params.proto]);
        ESP_ERROR_CHECK(hid_host_device_close(hid_device_handle));
        break;
        
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGI(TAG, "HID Device, protocol '%s' TRANSFER_ERROR",
                 hid_proto_name_str[dev_params.proto]);
        break;
        
    default:
        ESP_LOGE(TAG, "HID Device, protocol '%s' Unhandled event",
                 hid_proto_name_str[dev_params.proto]);
        break;
    }
}

/**
 * @brief HID host device event handler
 */
static void hid_host_device_event(hid_host_device_handle_t hid_device_handle,
                                const hid_host_driver_event_t event,
                                void *arg) {
    hid_host_dev_params_t dev_params;
    ESP_ERROR_CHECK(hid_host_device_get_params(hid_device_handle, &dev_params));

    switch (event) {
    case HID_HOST_DRIVER_EVENT_CONNECTED:
        ESP_LOGI(TAG, "HID Device, protocol '%s' CONNECTED",
                 hid_proto_name_str[dev_params.proto]);

        const hid_host_device_config_t dev_config = {
            .callback = hid_host_interface_callback,
            .callback_arg = NULL
        };

        ESP_ERROR_CHECK(hid_host_device_open(hid_device_handle, &dev_config));
        if (HID_SUBCLASS_BOOT_INTERFACE == dev_params.sub_class) {
            ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_device_handle, HID_REPORT_PROTOCOL_BOOT));
            if (HID_PROTOCOL_KEYBOARD == dev_params.proto) {
                ESP_ERROR_CHECK(hid_class_request_set_idle(hid_device_handle, 0, 0));
            }
        }
        ESP_ERROR_CHECK(hid_host_device_start(hid_device_handle));
        break;
        
    default:
        break;
    }
}

/**
 * @brief USB library thread
 */
static void* usb_lib_thread(void *arg) {
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    sem_post(&usb_task_semaphore);

    ESP_LOGI(TAG, "USB main loop started");
    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB shutdown");
    usleep(10 * 1000);
    ESP_ERROR_CHECK(usb_host_uninstall());
    pthread_exit(NULL);
}

/**
 * @brief GPIO interrupt handler for quit button
 */
static void gpio_isr_cb(void *arg) {
    BaseType_t xTaskWoken = pdFALSE;
    const app_event_queue_t evt_queue = {
        .event_group = APP_EVENT,
    };

    if (app_event_queue) {
        xQueueSendFromISR(app_event_queue, &evt_queue, &xTaskWoken);
    }

    if (xTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief HID host device callback
 */
static void hid_host_device_callback(hid_host_device_handle_t hid_device_handle,
                                   const hid_host_driver_event_t event,
                                   void *arg) {
    const app_event_queue_t evt_queue = {
        .event_group = APP_EVENT_HID_HOST,
        .hid_host_device.handle = hid_device_handle,
        .hid_host_device.event = event,
        .hid_host_device.arg = arg
    };

    if (app_event_queue) {
        xQueueSend(app_event_queue, &evt_queue, 0);
    }
}

/**
 * @brief USB event handler thread
 */
static void* usb_event_handler_thread(void* arg) {
    ESP_LOGI(TAG, "USB HID event handler started");

    while (keyboard_initialized) {
        esp_err_t ret = hid_host_handle_events(portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Error handling HID events: %d", ret);
            break;
        }
    }

    ESP_LOGI(TAG, "USB HID event handler shutting down");
    pthread_exit(NULL);
}

// Public API implementation

esp_err_t init_keyboard(void) {
    if (keyboard_initialized) {
        ESP_LOGW(TAG, "Keyboard already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing USB HID keyboard");

    // Add virtual SDL keyboard
    SDL_AddKeyboard(1, "ESP32 USB Keyboard");

    // Initialize GPIO for quit button
    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1));
    ESP_ERROR_CHECK(gpio_isr_handler_add(APP_QUIT_PIN, gpio_isr_cb, NULL));

    // Initialize semaphore
    sem_init(&usb_task_semaphore, 0, 0);

    // Create USB library thread
    pthread_t usb_thread;
    pthread_attr_t usb_thread_attr;
    pthread_attr_init(&usb_thread_attr);
    pthread_attr_setstacksize(&usb_thread_attr, 8912);

    int ret = pthread_create(&usb_thread, &usb_thread_attr, usb_lib_thread, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to create USB thread: %d", ret);
        return ESP_FAIL;
    }
    pthread_detach(usb_thread);

    // Wait for USB initialization
    sem_wait(&usb_task_semaphore);

    // Configure HID host driver
    const hid_host_driver_config_t hid_host_driver_config = {
        .create_background_task = false,
        .task_priority = 5,
        .stack_size = 8912,
        .core_id = 0,
        .callback = hid_host_device_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(hid_host_install(&hid_host_driver_config));

    // Create event queue
    app_event_queue = xQueueCreate(10, sizeof(app_event_queue_t));

    ESP_LOGI(TAG, "Waiting for HID devices to be connected");

    // Start HID event handler thread
    ret = pthread_create(&usb_event_thread, NULL, usb_event_handler_thread, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to create HID event handler thread: %d", ret);
        return ESP_FAIL;
    }
    pthread_detach(usb_event_thread);

    keyboard_initialized = true;
    ESP_LOGI(TAG, "USB HID keyboard initialized successfully");
    return ESP_OK;
}

void process_keyboard(void) {
    if (!keyboard_initialized || !app_event_queue) {
        return;
    }

    app_event_queue_t evt_queue;
    if (xQueueReceive(app_event_queue, &evt_queue, 0)) {
        if (evt_queue.event_group == APP_EVENT_HID_HOST) {
            hid_host_device_event(evt_queue.hid_host_device.handle,
                                 evt_queue.hid_host_device.event,
                                 evt_queue.hid_host_device.arg);
        }
    }
}

bool is_keyboard_available(void) {
    return keyboard_initialized;
}

void cleanup_keyboard(void) {
    if (!keyboard_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Cleaning up keyboard resources");
    keyboard_initialized = false;

    // Cleanup resources
    if (app_event_queue) {
        vQueueDelete(app_event_queue);
        app_event_queue = NULL;
    }

    // Remove GPIO interrupt
    gpio_isr_handler_remove(APP_QUIT_PIN);

    ESP_LOGI(TAG, "Keyboard cleanup completed");
}

#else // !CONFIG_IDF_TARGET_ESP32P4

// Stub implementations for non-ESP32P4 targets
esp_err_t init_keyboard(void) {
    ESP_LOGW(TAG, "USB HID keyboard only available on ESP32-P4");
    return ESP_ERR_NOT_SUPPORTED;
}

void process_keyboard(void) {
    // No-op on non-ESP32P4 targets
}

bool is_keyboard_available(void) {
    return false;
}

void cleanup_keyboard(void) {
    // No-op on non-ESP32P4 targets
}

#endif // CONFIG_IDF_TARGET_ESP32P4