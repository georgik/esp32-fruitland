/**************************************************************
 * Fruit Land - ESP32-S3-BOX Port                             *
 *                                                            *
 * Original Coding by: Arjan Bakker                           *
 * SDL3/ESP32 Port by: Juraj Michalek - https://georgik.rocks *
 **************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "SDL3/SDL.h"
#include "SDL3/SDL_hints.h"
#include "SDL_hints.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "filesystem.h"
#include "keyboard.h"
#include "accelerometer.h"
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "SDL3/SDL_esp-idf.h"  // For PPA hardware scaling
#include "driver/ppa.h"         // Hardware acceleration
#include "esp_lcd_panel_ops.h"  // Direct LCD operations
#endif

// Game constants
#define GAME_WIDTH 256
#define GAME_HEIGHT 224
#define SPRITE_SIZE 16
#define LEVEL_WIDTH 15
#define LEVEL_HEIGHT 11
#define MAX_OBJECTS 16
#define HI_ENTRIES 8
#define NAME_LENGTH 9

// Performance constants - tile-based movement system
#ifdef CONFIG_IDF_TARGET_ESP32P4
#define TARGET_FPS 60  // Higher FPS possible on ESP32-P4
#define TILE_MOVEMENT_DURATION_US 100000  // 0.1 seconds per tile movement (much more responsive)
#else
#define TARGET_FPS 30  // Increased FPS for ESP32-S3 after RGB565 optimization
#define TILE_MOVEMENT_DURATION_US 120000  // 0.12 seconds per tile movement (more responsive)
#endif
#define FRAME_TIME_US (1000000 / TARGET_FPS)
#define TILE_SIZE 16  // 16x16 pixel tiles
#define MOVEMENT_FRAMES (TILE_MOVEMENT_DURATION_US / FRAME_TIME_US)  // Frames per tile movement

// Enhanced animation constants
#define ANIMATION_FRAMES 4  // Number of walking animation frames per direction
#define IDLE_ANIMATION_FRAMES 2  // Number of idle animation frames
#define ANIMATION_SPEED_MS 100  // Milliseconds per animation frame (10 FPS animation)

// Optimized buffer configuration for ESP32
#define RENDER_BUFFER_HEIGHT 32  // Smaller chunks = less memory transfers
#define RENDER_BUFFER_SIZE (GAME_WIDTH * RENDER_BUFFER_HEIGHT)
#define USE_MINIMAL_UPDATES 1  // Only update what absolutely changed
#define SKIP_REDUNDANT_CLEARS 1  // Skip unnecessary clears

// Direction constants
#define UP    1
#define DOWN  2
#define LEFT  3
#define RIGHT 4

// Screen dimensions (will be set at runtime)
static int SCREEN_WIDTH = 320;
static int SCREEN_HEIGHT = 240;

// Game object structure
typedef struct OBJECT {
    int dx; // x-coordinate in level_data
    int dy; // y-coordinate in level_data
    int x; // x-coordinate on screen
    int y; // y-coordinate on screen
    int dir; // direction
    int step; // number of steps object has moved
    int l; // l==1 -> valid data in object
    int sx; // x-source data
    int sy; // y-source data
    // Tile-based movement fields
    int target_dx; // target x-coordinate in level_data
    int target_dy; // target y-coordinate in level_data
    int start_x; // starting screen x position for interpolation
    int start_y; // starting screen y position for interpolation
    int target_x; // target screen x position
    int target_y; // target screen y position
    uint64_t movement_start_time; // when movement started (microseconds)
    bool is_moving; // true if currently moving between tiles
    // Enhanced animation fields
    int current_frame; // current animation frame (0-3 for walking, 0-1 for idle)
    uint64_t last_anim_time; // last time animation frame changed
    int base_sy; // base sprite y-coordinate for current direction
} OBJECT;

// Global game state
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *intro_texture = NULL;
static SDL_Texture *patterns_texture = NULL;
static SDL_Texture *game_surface = NULL;

// High-performance streaming buffers
static SDL_Texture *render_line_buffer = NULL; // Small streaming buffer
static uint16_t *line_buffer_data = NULL; // Raw pixel data for line buffer
static bool use_streaming_render = true; // Enable streaming optimization
static bool skip_next_clear = false; // Skip redundant clears

// Game data
static char levels[4736]; // storage space for 25 levels
static char level_data[LEVEL_WIDTH * LEVEL_HEIGHT];
static OBJECT objects[MAX_OBJECTS];
// High scores (simplified for embedded version)
// static int hi_scores[HI_ENTRIES] = {100000,90000,80000,70000,60000,50000,30000,100};
// static char hi_names[HI_ENTRIES][NAME_LENGTH] = {
//     "ARJAN   ","ERWIN   ","KARIN   ","ADDY    ",
//     "GERRY   ","BAS     ","TOM     ","LAURENS "
// };

// Game variables
static int av_time;
static int score;
static int level;
static int lives;
static int fruit;
static int dead;
static int freeze_enemy;
static int level_change_requested = 0; // Flag to track F2/F3 level changes
static int game_running = 1;

#ifdef CONFIG_IDF_TARGET_ESP32P4
// ESP32-P4 hardware acceleration support
static ppa_client_handle_t ppa_handle = NULL;
static bool ppa_available = false;

// double buffering for ESP32-P4
static uint16_t *framebuf[2] = {NULL, NULL}; // Double framebuffers
static int current_fb = 0; // Current framebuffer index
static bool direct_framebuffer_mode = false; // Direct FB vs SDL mode
static esp_lcd_panel_handle_t lcd_panel = NULL;
static TaskHandle_t draw_task_handle = NULL;
static SemaphoreHandle_t fb_mutex = NULL;
static volatile bool fb_ready = false;
#endif

// Performance tracking
static uint64_t last_frame_time = 0;
static uint64_t frame_count = 0;
static bool full_redraw_needed = true;

// Performance tracking for optimization
static uint64_t total_render_time = 0;

// FPS measurement
static uint64_t fps_measurement_start_time = 0;
static uint64_t fps_frame_count = 0;

// Input state
static const bool *keyboard_state;

// Forward declarations
void print_stats(void);

void get_item(void);

bool is_passable(int tile_type);

void teleport(void);

void turn_screen(void);

void render_frame_minimal(void);

void update_character_animation(OBJECT *obj, bool is_moving);

// Rock and gravity system
int search_rock(int xr, int yr);

void move_rocks(void);

void move_block(void);

void init_block_sprite(void);

// Optimized area updates for minimal rendering
typedef struct {
    int start_line; // Starting line for update
    int line_count; // Number of lines to update
    bool full_update;
    bool needs_stats_update;
    bool needs_player_update;
} update_area_t;

static update_area_t pending_update = {0, 0, true, true, true};

// High-performance streaming render functions
void init_streaming_render() {
    if (render_line_buffer) {
        SDL_DestroyTexture(render_line_buffer);
    }
    if (line_buffer_data) {
        free(line_buffer_data);
    }

    // Create optimized texture for minimal updates
    render_line_buffer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                           SDL_TEXTUREACCESS_TARGET,
                                           GAME_WIDTH, RENDER_BUFFER_HEIGHT);

    // Allocate minimal pixel buffer (16-bit RGB565)
    line_buffer_data = (uint16_t *) malloc(RENDER_BUFFER_SIZE * sizeof(uint16_t));

    if (!render_line_buffer || !line_buffer_data) {
        ESP_LOGE("render", "Failed to create streaming buffers");
        use_streaming_render = false;
    } else {
        ESP_LOGI("render", "High-performance streaming initialized: %dx%d buffer",
                 GAME_WIDTH, RENDER_BUFFER_HEIGHT);
    }
}

void cleanup_streaming_render() {
    if (render_line_buffer) {
        SDL_DestroyTexture(render_line_buffer);
        render_line_buffer = NULL;
    }
    if (line_buffer_data) {
        free(line_buffer_data);
        line_buffer_data = NULL;
    }
}

// Smart change tracking for minimal updates
void mark_area_dirty(int y, int height) {
    if (pending_update.full_update) {
        return; // Already full update scheduled
    }

    if (pending_update.line_count == 0) {
        // First dirty area
        pending_update.start_line = y;
        pending_update.line_count = height;
    } else {
        // Merge with existing dirty area only if nearby
        int end_line = pending_update.start_line + pending_update.line_count;
        int new_end_line = y + height;

        // Only merge if areas are close (within 32 lines)
        if (abs(y - end_line) < 32) {
            pending_update.start_line = (y < pending_update.start_line) ? y : pending_update.start_line;
            end_line = (new_end_line > end_line) ? new_end_line : end_line;
            pending_update.line_count = end_line - pending_update.start_line;
        }
    }
}

void mark_full_update() {
    pending_update.full_update = true;
    pending_update.start_line = 0;
    pending_update.line_count = GAME_HEIGHT;
    pending_update.needs_stats_update = true;
    pending_update.needs_player_update = true;
}

void mark_player_update() {
    pending_update.needs_player_update = true;
}

void mark_stats_update() {
    pending_update.needs_stats_update = true;
}

// Frame skipping for performance
bool should_skip_frame() {
    static int frame_skip_counter = 0;

    // Skip every 3rd frame when only stats change
    if (!pending_update.needs_player_update && pending_update.needs_stats_update) {
        frame_skip_counter++;
        if (frame_skip_counter % 3 == 0) {
            return true;
        }
    }

    return false;
}

// Performance timing functions
uint64_t get_time_us() {
    return esp_timer_get_time();
}

// Linear interpolation for smooth sliding movement (0.0 to 1.0)
float interpolate_movement(uint64_t start_time, uint64_t current_time) {
    uint64_t elapsed = current_time - start_time;
    if (elapsed >= TILE_MOVEMENT_DURATION_US) {
        return 1.0f; // Movement complete
    }

    // Pure linear interpolation - no easing, smooth sliding motion
    float t = (float) elapsed / TILE_MOVEMENT_DURATION_US;

    return t;
}

// Enhanced character animation system
void update_character_animation(OBJECT *obj, bool is_moving) {
    uint64_t current_time = get_time_us();
    uint64_t elapsed_since_last_anim = current_time - obj->last_anim_time;

    // Convert animation speed from milliseconds to microseconds
    uint64_t animation_interval = ANIMATION_SPEED_MS * 1000;

    // Check if it's time to advance the animation frame
    if (elapsed_since_last_anim >= animation_interval) {
        if (is_moving) {
            // Walking animation - cycle through 4 frames
            obj->current_frame = (obj->current_frame + 1) % ANIMATION_FRAMES;
        } else {
            // Idle animation - gentle breathing/standing animation with 2 frames
            obj->current_frame = (obj->current_frame + 1) % IDLE_ANIMATION_FRAMES;
        }
        obj->last_anim_time = current_time;
    }

    // Calculate sprite coordinates based on direction and animation frame
    if (is_moving) {
        // Walking animation: each direction has 4 frames horizontally
        obj->sx = obj->current_frame * 16;
        obj->sy = obj->base_sy; // Use the base y-coordinate for current direction
    } else {
        // Idle animation: alternate between frame 0 and 1 for subtle breathing
        obj->sx = obj->current_frame * 16;
        obj->sy = obj->base_sy; // Keep same direction when idle
    }
}

// Update player position with continuous smooth movement
void update_player_position() {
    if (!objects[0].is_moving) {
        // Not moving, but still update idle animation
        update_character_animation(&objects[0], false); // Moving = false
        return;
    }

    uint64_t current_time = get_time_us();
    float progress = interpolate_movement(objects[0].movement_start_time, current_time);

    if (progress >= 1.0f) {
        // Movement to current target completed
        objects[0].x = objects[0].target_x;
        objects[0].y = objects[0].target_y;
        objects[0].dx = objects[0].target_dx;
        objects[0].dy = objects[0].target_dy;

        // Handle item collection at destination
        get_item();

        // Check if we should continue moving in the same direction
        bool continue_movement = false;
        int next_target_dx = objects[0].dx;
        int next_target_dy = objects[0].dy;

        // Check if the same direction key is still pressed and next tile is passable
        if (objects[0].dir == UP && keyboard_state[SDL_SCANCODE_UP] && objects[0].dy > 0) {
            next_target_dy = objects[0].dy - 1;
            continue_movement = true;
        } else if (objects[0].dir == DOWN && keyboard_state[SDL_SCANCODE_DOWN] && objects[0].dy < LEVEL_HEIGHT - 1) {
            next_target_dy = objects[0].dy + 1;
            continue_movement = true;
        } else if (objects[0].dir == LEFT && keyboard_state[SDL_SCANCODE_LEFT] && objects[0].dx > 0) {
            next_target_dx = objects[0].dx - 1;
            continue_movement = true;
        } else if (objects[0].dir == RIGHT && keyboard_state[SDL_SCANCODE_RIGHT] && objects[0].dx < LEVEL_WIDTH - 1) {
            next_target_dx = objects[0].dx + 1;
            continue_movement = true;
        }

        if (continue_movement) {
            // Check if next tile is passable
            int next_tile = level_data[next_target_dx + next_target_dy * LEVEL_WIDTH];
            if (is_passable(next_tile)) {
                // Continue smooth movement to next tile
                objects[0].target_dx = next_target_dx;
                objects[0].target_dy = next_target_dy;
                objects[0].start_x = objects[0].x;
                objects[0].start_y = objects[0].y;
                objects[0].target_x = next_target_dx * 16 + 8;
                objects[0].target_y = next_target_dy * 16 + 8;
                objects[0].movement_start_time = current_time; // Start new movement immediately
                // Keep is_moving = true and dir unchanged for continuous movement
            } else {
                // Stop movement - blocked
                objects[0].is_moving = false;
                objects[0].dir = 0;
            }
        } else {
            // Stop movement - key released or different direction
            objects[0].is_moving = false;
            objects[0].dir = 0;
        }
    } else {
        // Interpolate between start and target positions
        objects[0].x = objects[0].start_x + (int) ((objects[0].target_x - objects[0].start_x) * progress);
        objects[0].y = objects[0].start_y + (int) ((objects[0].target_y - objects[0].start_y) * progress);

        // Enhanced smooth walking animation
        update_character_animation(&objects[0], true); // Moving = true
    }
}

void wait_for_frame_time() {
    uint64_t current_time = get_time_us();
    uint64_t elapsed = current_time - last_frame_time;

    // Initialize FPS measurement on first frame
    if (fps_measurement_start_time == 0) {
        fps_measurement_start_time = current_time;
        fps_frame_count = 0;
    }

    // Tile-based frame timing - target FPS control
    if (elapsed < FRAME_TIME_US) {
        uint64_t sleep_time = FRAME_TIME_US - elapsed;
        if (sleep_time > 1000) {
            // Sleep if > 1ms
            vTaskDelay(pdMS_TO_TICKS((sleep_time / 1000) + 1));
        }
    }

    last_frame_time = get_time_us();
    frame_count++;
    fps_frame_count++;

    // Calculate and log real FPS every 10 seconds
    uint64_t fps_elapsed = current_time - fps_measurement_start_time;
    if (fps_elapsed >= 10000000) {
        // 10 seconds in microseconds
        float actual_fps = (float) fps_frame_count * 1000000.0f / (float) fps_elapsed;
        uint64_t avg_frame_time = total_render_time / (frame_count > 0 ? frame_count : 1);

        ESP_LOGI("FPS", "🎮 ACTUAL FPS: %.1f | TARGET: %d | AVG FRAME TIME: %llu us",
                 actual_fps, TARGET_FPS, avg_frame_time);
        ESP_LOGI("FPS", "📊 Frames: %llu in 10s | Frame budget: %llu us",
                 fps_frame_count, FRAME_TIME_US);

        // Reset measurement
        fps_measurement_start_time = current_time;
        fps_frame_count = 0;
    }
}

#ifdef CONFIG_IDF_TARGET_ESP32P4
// Hybrid drawing task - uses SDL for display but direct pixels for speed
static void draw_task(void *param) {
    ESP_LOGI("fb_draw", "Hybrid framebuffer drawing task started on core 1");

    while (true) {
        // Wait for notification that frame is ready
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (direct_framebuffer_mode && framebuf[current_fb]) {
            xSemaphoreTake(fb_mutex, portMAX_DELAY);

            // Convert framebuffer to SDL surface and display (hybrid approach)
            // This gives us the speed of direct pixel manipulation with SDL compatibility
            if (game_surface && renderer) {
                void *pixels;
                int pitch;
                if (SDL_LockTexture(game_surface, NULL, &pixels, &pitch) == 0) {
                    // Fast memcpy from our framebuffer to SDL texture
                    uint32_t *dst = (uint32_t *) pixels;
                    uint16_t *src = framebuf[current_fb];

                    // Convert RGB565 to RGBA8888 efficiently
                    for (int i = 0; i < GAME_WIDTH * GAME_HEIGHT; i++) {
                        uint16_t rgb565 = src[i];
                        uint8_t r = (rgb565 >> 8) & 0xF8;
                        uint8_t g = (rgb565 >> 3) & 0xFC;
                        uint8_t b = (rgb565 << 3) & 0xF8;
                        dst[i] = (0xFF << 24) | (r << 16) | (g << 8) | b; // RGBA
                    }

                    SDL_UnlockTexture(game_surface);
                }
            }

            // Ping-pong buffer switch
            current_fb = current_fb ? 0 : 1;
            fb_ready = true;

            xSemaphoreGive(fb_mutex);
        }
    }
}

// Initialize framebuffers
static esp_err_t init_direct_framebuffer() {
    ESP_LOGI("fb_init", "Initializing double framebuffers");

    // Allocate DMA-capable SPIRAM framebuffers
    size_t fb_size = GAME_WIDTH * GAME_HEIGHT * sizeof(uint16_t);

    framebuf[0] = heap_caps_calloc(1, fb_size, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    framebuf[1] = heap_caps_calloc(1, fb_size, MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);

    if (!framebuf[0] || !framebuf[1]) {
        ESP_LOGE("fb_init", "Failed to allocate DMA framebuffers");
        if (framebuf[0]) free(framebuf[0]);
        if (framebuf[1]) free(framebuf[1]);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI("fb_init", "Allocated framebuffers: %p, %p (size: %zu bytes each)",
             framebuf[0], framebuf[1], fb_size);

    // Create mutex for framebuffer access
    fb_mutex = xSemaphoreCreateMutex();
    if (!fb_mutex) {
        ESP_LOGE("fb_init", "Failed to create framebuffer mutex");
        return ESP_ERR_NO_MEM;
    }

    // Create high-priority drawing task on core 1
    BaseType_t ret = xTaskCreatePinnedToCore(draw_task, "fb_draw", 4096, NULL,
                                             5, &draw_task_handle, 1);
    if (ret != pdPASS) {
        ESP_LOGE("fb_init", "Failed to create drawing task");
        return ESP_FAIL;
    }

    direct_framebuffer_mode = true;
    current_fb = 0;
    fb_ready = true;

    ESP_LOGI("fb_init", "Direct framebuffer mode initialized successfully");
    return ESP_OK;
}

// Cleanup direct framebuffer
static void cleanup_direct_framebuffer() {
    direct_framebuffer_mode = false;

    if (draw_task_handle) {
        vTaskDelete(draw_task_handle);
        draw_task_handle = NULL;
    }

    if (fb_mutex) {
        vSemaphoreDelete(fb_mutex);
        fb_mutex = NULL;
    }

    if (framebuf[0]) {
        free(framebuf[0]);
        framebuf[0] = NULL;
    }

    if (framebuf[1]) {
        free(framebuf[1]);
        framebuf[1] = NULL;
    }

    ESP_LOGI("fb_cleanup", "Direct framebuffer mode cleaned up");
}

// Initialize ESP32-P4 hardware acceleration
void init_p4_acceleration() {
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM, // Scale, Rotate, Mirror operations
    };

    esp_err_t ret = ppa_register_client(&ppa_cfg, &ppa_handle);
    if (ret == ESP_OK) {
        ppa_available = true;
        ESP_LOGI("p4_accel", "PPA hardware acceleration initialized successfully");
    } else {
        ppa_available = false;
        ESP_LOGW("p4_accel", "PPA hardware acceleration not available: %s", esp_err_to_name(ret));
    }

    // Temporarily disable direct framebuffer due to memory access issues
    // Use optimized SDL rendering instead for stable performance
    direct_framebuffer_mode = false;
    ESP_LOGI("p4_accel", "Direct framebuffer disabled - using optimized SDL rendering");
}

void cleanup_p4_acceleration() {
    cleanup_direct_framebuffer();

    if (ppa_available && ppa_handle) {
        ppa_unregister_client(ppa_handle);
        ppa_handle = NULL;
        ppa_available = false;
        ESP_LOGI("p4_accel", "PPA hardware acceleration cleaned up");
    }
}
// Direct framebuffer drawing functions
static void fb_draw_pixel(int x, int y, uint16_t color) {
    if (x >= 0 && x < GAME_WIDTH && y >= 0 && y < GAME_HEIGHT && framebuf[current_fb]) {
        framebuf[current_fb][y * GAME_WIDTH + x] = color;
    }
}

static void fb_draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (!framebuf[current_fb]) return;

    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            fb_draw_pixel(x + dx, y + dy, color);
        }
    }
}

static void fb_clear(uint16_t color) {
    if (!framebuf[current_fb]) return;

    size_t pixels = GAME_WIDTH * GAME_HEIGHT;
    for (size_t i = 0; i < pixels; i++) {
        framebuf[current_fb][i] = color;
    }
}

// Convert 24-bit RGB to 16-bit RGB565
static inline uint16_t rgb_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Fast sprite drawing to framebuffer (much faster than SDL)
static void fb_draw_sprite(int dst_x, int dst_y, int src_x, int src_y, int w, int h, SDL_Surface *sprite_surface) {
    if (!framebuf[current_fb] || !sprite_surface || !sprite_surface->pixels) return;

    uint32_t *src_pixels = (uint32_t *) sprite_surface->pixels;
    int src_pitch = sprite_surface->pitch / 4; // Assuming RGBA format

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int screen_x = dst_x + x;
            int screen_y = dst_y + y;

            // Bounds checking
            if (screen_x < 0 || screen_x >= GAME_WIDTH ||
                screen_y < 0 || screen_y >= GAME_HEIGHT)
                continue;

            int sprite_x = src_x + x;
            int sprite_y = src_y + y;
            if (sprite_x >= sprite_surface->w || sprite_y >= sprite_surface->h) continue;

            uint32_t rgba = src_pixels[sprite_y * src_pitch + sprite_x];
            uint8_t a = (rgba >> 24) & 0xFF;

            // Skip transparent pixels
            if (a == 0) continue;

            uint8_t r = (rgba >> 16) & 0xFF;
            uint8_t g = (rgba >> 8) & 0xFF;
            uint8_t b = rgba & 0xFF;

            uint16_t rgb565 = rgb_to_rgb565(r, g, b);
            framebuf[current_fb][screen_y * GAME_WIDTH + screen_x] = rgb565;
        }
    }
}

// Fast level tile drawing to framebuffer
static void fb_draw_level_tile(int tile_x, int tile_y, int tile_type) {
    if (!framebuf[current_fb]) return;

    int screen_x = tile_x * 16 + 8;
    int screen_y = tile_y * 16 + 8;

    uint16_t tile_color;
    switch (tile_type) {
        case 0: return; // Empty - skip drawing
        case 1: tile_color = rgb_to_rgb565(255, 255, 0);
            break; // Dot - yellow
        case 2: tile_color = rgb_to_rgb565(0, 0, 255);
            break; // Wall - blue
        case 3: tile_color = rgb_to_rgb565(139, 69, 19);
            break; // Rock - brown
        case 4: tile_color = rgb_to_rgb565(255, 0, 0);
            break; // Fruit - red
        case 11: tile_color = rgb_to_rgb565(192, 192, 192);
            break; // Stone block - light gray
        default: tile_color = rgb_to_rgb565(128, 128, 128);
            break; // Other - gray
    }

    // Draw 16x16 tile
    for (int dy = 0; dy < 16; dy++) {
        for (int dx = 0; dx < 16; dx++) {
            int px = screen_x + dx;
            int py = screen_y + dy;
            if (px >= 0 && px < GAME_WIDTH && py >= 0 && py < GAME_HEIGHT) {
                framebuf[current_fb][py * GAME_WIDTH + px] = tile_color;
            }
        }
    }
}

// Trigger framebuffer display
static void fb_present() {
    if (direct_framebuffer_mode && draw_task_handle) {
        xTaskNotifyGive(draw_task_handle);
    }
}

// Get LCD panel handle from SDL BSP for direct access
static void fb_get_lcd_panel() {
    ESP_LOGI("fb_panel", "Attempting to get LCD panel handle from BSP");

    /* TODO: Proper LCD panel connection for direct framebuffer
     *
     * To properly connect the direct framebuffer to the display, we need to:
     *
     * 1. Get the actual LCD panel handle from the ESP32-P4 BSP:
     *    - The BSP initializes esp_lcd_panel_handle_t internally
     *    - We need to expose this handle via BSP API or SDL layer
     *
     * 2. Modify the draw task to use esp_lcd_panel_draw_bitmap():
     *    - Convert our RGB565 framebuffer to the panel's expected format
     *    - Call esp_lcd_panel_draw_bitmap(lcd_panel, 0, 0, width, height, framebuf)
     *
     * 3. Handle coordinate scaling:
     *    - Our game renders at 256x224 but display is 1024x600
     *    - Either scale the framebuffer or adjust coordinates
     *
     * For now, using SDL fallback which works perfectly.
     */

    lcd_panel = NULL; // Will be set when we have proper BSP integration
    ESP_LOGI("fb_panel", "LCD panel connection pending - using SDL display path");
}
#endif

void init_render_system() {
    init_streaming_render();
#ifdef CONFIG_IDF_TARGET_ESP32P4
    init_p4_acceleration();
    if (direct_framebuffer_mode) {
        fb_get_lcd_panel();
        ESP_LOGI("game", "Render system initialized for ESP32-P4");
    } else {
        ESP_LOGI("game", "Render system initialized for ESP32-P4 (SDL fallback)");
    }
#else
    ESP_LOGI("game", "Render system initialized for ESP32-S3 (software rendering)");
#endif

    // Display FPS and performance targets
    ESP_LOGI("FPS", "🎯 TARGET FPS: %d | Frame budget: %llu us (%.2f ms)",
             TARGET_FPS, FRAME_TIME_US, FRAME_TIME_US / 1000.0f);
#ifdef CONFIG_IDF_TARGET_ESP32P4
    ESP_LOGI("FPS", "⚡ ESP32-P4 HIGH PERFORMANCE MODE - Targeting 60 FPS");
#else
    ESP_LOGI("FPS", "🐢 ESP32-S3 CONSERVATIVE MODE - Targeting 20 FPS");
#endif

    ESP_LOGI("game", "Starting game...");
    ESP_LOGI("debug", "🎮 Level Navigation: F2 = Previous Level | F3 = Next Level | ESC = Exit");
}

void cleanup_render_system() {
    cleanup_streaming_render();
#ifdef CONFIG_IDF_TARGET_ESP32P4
    cleanup_p4_acceleration();
#endif
}

#ifdef CONFIG_IDF_TARGET_ESP32P4
// high-performance framebuffer render function
void render_frame_direct_fb() {
    if (!direct_framebuffer_mode || !framebuf[current_fb]) {
        // Fallback to SDL rendering
        render_frame_minimal();
        return;
    }

    // Wait for framebuffer to be ready
    if (!fb_ready) {
        return;
    }

    xSemaphoreTake(fb_mutex, portMAX_DELAY);

    // Clear framebuffer (black background)
    fb_clear(rgb_to_rgb565(0, 0, 0));

    // Render actual game content to framebuffer
    // Note: This is a simplified direct rendering - full version would need
    // to replicate all SDL drawing operations

    // Draw level tiles (simplified - just walls and empty space)
    for (int y = 0; y < LEVEL_HEIGHT; y++) {
        for (int x = 0; x < LEVEL_WIDTH; x++) {
            int tile = level_data[y * LEVEL_WIDTH + x];
            uint16_t tile_color;

            switch (tile) {
                case 0: tile_color = rgb_to_rgb565(0, 0, 0);
                    break; // Empty - black
                case 1: tile_color = rgb_to_rgb565(255, 255, 0);
                    break; // Dot - yellow
                case 2: tile_color = rgb_to_rgb565(0, 0, 255);
                    break; // Wall - blue
                case 3: tile_color = rgb_to_rgb565(139, 69, 19);
                    break; // Rock - brown
                case 4: tile_color = rgb_to_rgb565(255, 0, 0);
                    break; // Fruit - red
                default: tile_color = rgb_to_rgb565(128, 128, 128);
                    break; // Other - gray
            }

            if (tile != 0) {
                // Don't draw empty tiles
                fb_draw_rect(x * 16 + 8, y * 16 + 8, 16, 16, tile_color);
            }
        }
    }

    // Draw player
    if (objects[0].l) {
        uint16_t player_color = rgb_to_rgb565(255, 255, 0); // Yellow
        fb_draw_rect(objects[0].x, objects[0].y, 16, 16, player_color);
    }

    // Draw rocks
    for (int r = 5; r < 15; r++) {
        if (objects[r].l) {
            uint16_t rock_color = rgb_to_rgb565(139, 69, 19); // Brown
            fb_draw_rect(objects[r].x, objects[r].y, 16, 16, rock_color);
        }
    }

    fb_ready = false;
    xSemaphoreGive(fb_mutex);

    // Trigger display update
    fb_present();

    // Clear update flags
    pending_update.line_count = 0;
    pending_update.full_update = false;
    pending_update.needs_stats_update = false;
    pending_update.needs_player_update = false;
}
#endif

// Ultra-fast minimal render function for 2x performance
void render_frame_minimal() {
    // Skip frame if nothing significant changed
    if (should_skip_frame()) {
        return;
    }

    static bool first_render = true;
    static float cached_scale = 0;
    static int cached_offset_x = 0, cached_offset_y = 0;
    static int cached_scaled_w = 0, cached_scaled_h = 0;

    // Cache scaling calculations (only once)
    if (cached_scale == 0) {
        float scale_x = (float) SCREEN_WIDTH / GAME_WIDTH;
        float scale_y = (float) SCREEN_HEIGHT / GAME_HEIGHT;
        cached_scale = (scale_x < scale_y) ? scale_x : scale_y;

        cached_scaled_w = GAME_WIDTH * cached_scale;
        cached_scaled_h = GAME_HEIGHT * cached_scale;
        cached_offset_x = (SCREEN_WIDTH - cached_scaled_w) / 2;
        cached_offset_y = (SCREEN_HEIGHT - cached_scaled_h) / 2;
    }

    // Only clear screen on first render or level change
    if (first_render || pending_update.full_update) {
        SDL_SetRenderTarget(renderer, NULL);
        if (!skip_next_clear) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
        }
        skip_next_clear = true;
        first_render = false;
    }

    // Direct render to screen for maximum speed
    SDL_SetRenderTarget(renderer, NULL);
    SDL_FRect dst_rect = {cached_offset_x, cached_offset_y, cached_scaled_w, cached_scaled_h};
    SDL_RenderTexture(renderer, game_surface, NULL, &dst_rect);

    // Clear update flags
    pending_update.line_count = 0;
    pending_update.full_update = false;
    pending_update.needs_stats_update = false;
    pending_update.needs_player_update = false;
}

// Simple render function - fallback when streaming is disabled
void render_frame_simple() {
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    // Calculate scaling
    float scale_x = (float) SCREEN_WIDTH / GAME_WIDTH;
    float scale_y = (float) SCREEN_HEIGHT / GAME_HEIGHT;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;

    int scaled_w = GAME_WIDTH * scale;
    int scaled_h = GAME_HEIGHT * scale;
    int offset_x = (SCREEN_WIDTH - scaled_w) / 2;
    int offset_y = (SCREEN_HEIGHT - scaled_h) / 2;

    SDL_FRect dst_rect = {offset_x, offset_y, scaled_w, scaled_h};
    SDL_RenderTexture(renderer, game_surface, NULL, &dst_rect);
}

// Load game assets
int load_assets() {
    // Load level data
    FILE *levdat = fopen("/assets/fruit.dat", "rb");
    if (!levdat) {
        printf("Failed to open /assets/fruit.dat\n");
        return 0;
    }
    fread(levels, 1, 4736, levdat);
    fclose(levdat);

    // Load intro bitmap (convert to RGB565 for faster blit on embedded)
    SDL_Surface *intro_surface = SDL_LoadBMP("/assets/intro.bmp");
    if (!intro_surface) {
        printf("Failed to load /assets/intro.bmp: %s\n", SDL_GetError());
        return 0;
    }
    SDL_Surface *intro565 = SDL_ConvertSurface(intro_surface, SDL_PIXELFORMAT_RGB565);
    SDL_DestroySurface(intro_surface);
    if (!intro565) {
        printf("Failed to convert intro to RGB565: %s\n", SDL_GetError());
        return 0;
    }
    intro_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC,
                                      intro565->w, intro565->h);
    if (!intro_texture) {
        printf("Failed to create intro texture: %s\n", SDL_GetError());
        SDL_DestroySurface(intro565);
        return 0;
    }
    SDL_UpdateTexture(intro_texture, NULL, intro565->pixels, intro565->pitch);
    SDL_DestroySurface(intro565);

    // Load patterns bitmap and convert to RGB565
    SDL_Surface *patterns_surface = SDL_LoadBMP("/assets/patterns.bmp");
    if (!patterns_surface) {
        printf("Failed to load /assets/patterns.bmp: %s\n", SDL_GetError());
        return 0;
    }
    SDL_Surface *patterns565 = SDL_ConvertSurface(patterns_surface, SDL_PIXELFORMAT_RGB565);
    SDL_DestroySurface(patterns_surface);
    if (!patterns565) {
        printf("Failed to convert patterns to RGB565: %s\n", SDL_GetError());
        return 0;
    }
    patterns_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STATIC,
                                         patterns565->w, patterns565->h);
    if (!patterns_texture) {
        printf("Failed to create patterns texture: %s\n", SDL_GetError());
        SDL_DestroySurface(patterns565);
        return 0;
    }
    SDL_UpdateTexture(patterns_texture, NULL, patterns565->pixels, patterns565->pitch);
    SDL_DestroySurface(patterns565);

    // Create game surface texture for off-screen rendering in RGB565
    game_surface = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                     SDL_TEXTUREACCESS_TARGET, GAME_WIDTH, GAME_HEIGHT);
    if (!game_surface) {
        printf("Failed to create game surface: %s\n", SDL_GetError());
        return 0;
    }

    return 1;
}

// Print number on screen
void print_number(int px, int py, int n, int s) {
    SDL_SetRenderTarget(renderer, game_surface);
    px = px + 8 * (s - 1);
    for (int c = 0; c < s; c++) {
        SDL_FRect src_rect = {(n % 10) * 8 + 48, 8, 8, 8};
        SDL_FRect dst_rect = {px, py, 8, 8};
        SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
        n = n / 10;
        px = px - 8;
    }
}

// Draw text using pattern font
void draw_text_out(int x, int y, const char *s) {
    SDL_SetRenderTarget(renderer, game_surface);
    for (int c = 0; c < strlen(s); c++) {
        int sx, sy;
        if (s[c] == ':') {
            sx = 128;
            sy = 8;
        } else {
            sx = (int) (s[c] - 65) * 8 + 48;
            sy = 0;
        }
        SDL_FRect src_rect = {sx, sy, 8, 8};
        SDL_FRect dst_rect = {x + c * 8, y, 8, 8};
        SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
    }
}

// Draw game stats
void draw_texts() {
    draw_text_out(8, 192, "SCORE:");
    draw_text_out(8, 200, "TIME :");
    draw_text_out(176, 192, "LEVEL:");
    draw_text_out(176, 200, "LIVES:");
}

// Print game statistics
void print_stats() {
    print_number(64, 192, score, 8);
    print_number(64, 200, av_time, 4);
    print_number(232, 192, level, 2);
    print_number(232, 200, lives, 2);
}

// Draw level border
void draw_border() {
    SDL_SetRenderTarget(renderer, game_surface);
    for (int x = 8; x < 248; x += 16) {
        SDL_FRect src_rect = {16, 0, 16, 8};
        SDL_FRect dst_rect1 = {x, 0, 16, 8};
        SDL_FRect dst_rect2 = {x, 184, 16, 8};
        SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect1);
        src_rect.y = 8;
        SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect2);
    }

    for (int y = 8; y < 184; y += 16) {
        SDL_FRect src_rect1 = {0, 0, 8, 16};
        SDL_FRect src_rect2 = {8, 0, 8, 16};
        SDL_FRect dst_rect1 = {0, y, 8, 16};
        SDL_FRect dst_rect2 = {248, y, 8, 16};
        SDL_RenderTexture(renderer, patterns_texture, &src_rect1, &dst_rect1);
        SDL_RenderTexture(renderer, patterns_texture, &src_rect2, &dst_rect2);
    }

    // Corner pieces
    SDL_FRect corner_rects[4] = {{32, 0, 8, 8}, {40, 0, 8, 8}, {32, 8, 8, 8}, {40, 8, 8, 8}};
    SDL_FRect corner_positions[4] = {{0, 0, 8, 8}, {0, 184, 8, 8}, {248, 0, 8, 8}, {248, 184, 8, 8}};
    for (int i = 0; i < 4; i++) {
        SDL_RenderTexture(renderer, patterns_texture, &corner_rects[i], &corner_positions[i]);
    }
}

// Draw the game level
void draw_level() {
    SDL_SetRenderTarget(renderer, game_surface);
    for (int y = 0; y < LEVEL_HEIGHT; y++) {
        for (int x = 0; x < LEVEL_WIDTH; x++) {
            int tile = level_data[y * LEVEL_WIDTH + x];
            int xx = tile % 16;
            int yy = tile / 16;
            SDL_FRect src_rect = {(float) (xx * 16), (float) (yy * 16 + 16), 16.0f, 16.0f};
            SDL_FRect dst_rect = {(float) (x * 16 + 8), (float) (y * 16 + 8), 16.0f, 16.0f};
            SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
        }
    }
}

// Initialize level data
void init_level_data() {
    int a = (level - 1) * (LEVEL_WIDTH * LEVEL_HEIGHT + 4);

    // Binary Coded Decimal conversion for time
    av_time = (levels[a + 1] & 15) + ((levels[a + 1] & 240) >> 4) * 10;
    av_time = av_time + (levels[a] & 15) * 100 + ((levels[a] & 240) >> 4) * 1000;
    av_time = av_time + av_time / 2; // 50% extra time

    a = a + 4;
    for (int c = 0; c < LEVEL_WIDTH * LEVEL_HEIGHT; c++) {
        int d = levels[a + c];
        if (d == 15) d--; // Remove old enemy type
        level_data[c] = d;
    }
    a = a - 2;
    level_data[levels[a] * LEVEL_WIDTH + levels[a + 1]] = 32; // Player start position
}

// Count fruits in level
void count_fruit() {
    fruit = 0;
    for (int a = 0; a < LEVEL_WIDTH * LEVEL_HEIGHT; a++) {
        if (level_data[a] == 4) fruit++;
    }
}

// Clear game surface
void clear_game_surface() {
    SDL_SetRenderTarget(renderer, game_surface);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
}

// Global flag for level drawing optimization
static bool level_drawn = false;

// Print entire level with optimized drawing
void print_level() {
    // Only draw level once unless it's a new level
    if (!level_drawn) {
        clear_game_surface();
        draw_border();
        draw_level();
        draw_texts();
        level_drawn = true;

        // Force full redraw on level change
        full_redraw_needed = true;
        mark_full_update();
    }
}

// Reset level drawing flag for new level
void reset_level_drawing() {
    level_drawn = false;
    full_redraw_needed = true; // Force full redraw for new level
}

// Show intro screen
void show_intro() {
    SDL_SetRenderTarget(renderer, NULL);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    // Scale intro to fit screen while maintaining aspect ratio
    float scale_x = (float) SCREEN_WIDTH / GAME_WIDTH;
    float scale_y = (float) SCREEN_HEIGHT / GAME_HEIGHT;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;

    int scaled_w = GAME_WIDTH * scale;
    int scaled_h = GAME_HEIGHT * scale;
    int offset_x = (SCREEN_WIDTH - scaled_w) / 2;
    int offset_y = (SCREEN_HEIGHT - scaled_h) / 2;

    SDL_FRect dst_rect = {offset_x, offset_y, scaled_w, scaled_h};
    SDL_RenderTexture(renderer, intro_texture, NULL, &dst_rect);
    SDL_RenderPresent(renderer);
}

// Basic menu selection (simplified for embedded)
int select_option() {
    int option = 0;
    int confirmed = 0;

    while (!confirmed) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_KEY_DOWN) {
                switch (event.key.key) {
                    case SDLK_UP:
                        option = (option > 0) ? option - 1 : 3;
                        break;
                    case SDLK_DOWN:
                        option = (option < 3) ? option + 1 : 0;
                        break;
                    case SDLK_RETURN:
                    case SDLK_SPACE:
                        confirmed = 1;
                        break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(16));
    }
    return option;
}

// Initialize game objects
void init_objects() {
    // Reset all objects
    memset(objects, 0, sizeof(objects));

    // Find player start position
    int c = 0;
    while (level_data[c] != 32) c++;

    objects[0].dx = c % LEVEL_WIDTH;
    objects[0].dy = c / LEVEL_WIDTH;
    objects[0].x = (c % LEVEL_WIDTH) * 16 + 8;
    objects[0].y = (c / LEVEL_WIDTH) * 16 + 8;
    objects[0].l = 1;
    objects[0].sx = 0;
    objects[0].sy = 48;
    // Initialize tile-based movement fields
    objects[0].target_dx = objects[0].dx;
    objects[0].target_dy = objects[0].dy;
    objects[0].start_x = objects[0].x;
    objects[0].start_y = objects[0].y;
    objects[0].target_x = objects[0].x;
    objects[0].target_y = objects[0].y;
    objects[0].movement_start_time = 0;
    objects[0].is_moving = false;
    // Initialize enhanced animation fields
    objects[0].current_frame = 0;
    objects[0].last_anim_time = get_time_us();
    objects[0].base_sy = 48; // Default facing down
    level_data[c] = 0;

    // Initialize rocks and enemies
    int cur_rock = 5;
    int cur_enemy = 1;

    for (c = 0; c < LEVEL_WIDTH * LEVEL_HEIGHT; c++) {
        if (level_data[c] == 3 && cur_rock < 15) {
            // Rock - initialize with tile-based movement support
            objects[cur_rock].dx = c % LEVEL_WIDTH;
            objects[cur_rock].dy = c / LEVEL_WIDTH;
            objects[cur_rock].x = (c % LEVEL_WIDTH) * 16 + 8;
            objects[cur_rock].y = (c / LEVEL_WIDTH) * 16 + 8;
            objects[cur_rock].l = 1; // Stationary rock
            objects[cur_rock].sx = 48;
            objects[cur_rock].sy = 16;
            objects[cur_rock].dir = 0; // Not moving initially
            objects[cur_rock].is_moving = false;
            objects[cur_rock].target_dx = objects[cur_rock].dx;
            objects[cur_rock].target_dy = objects[cur_rock].dy;
            objects[cur_rock].target_x = objects[cur_rock].x;
            objects[cur_rock].target_y = objects[cur_rock].y;
            objects[cur_rock].start_x = objects[cur_rock].x;
            objects[cur_rock].start_y = objects[cur_rock].y;
            objects[cur_rock].movement_start_time = 0;
            ESP_LOGI("init", "Initialized rock %d at (%d,%d)", cur_rock, objects[cur_rock].dx, objects[cur_rock].dy);
            cur_rock++;
        }
        if ((level_data[c] == 14 || level_data[c] == 13) && cur_enemy < 5) {
            // Enemy
            objects[cur_enemy].dx = c % LEVEL_WIDTH;
            objects[cur_enemy].dy = c / LEVEL_WIDTH;
            objects[cur_enemy].x = (c % LEVEL_WIDTH) * 16 + 8;
            objects[cur_enemy].y = (c / LEVEL_WIDTH) * 16 + 8;
            objects[cur_enemy].l = (level_data[c] == 14) ? 1 : 2;
            objects[cur_enemy].sy = (level_data[c] == 14) ? 32 : 48;
            objects[cur_enemy].dir = (level_data[c] == 14) ? LEFT : UP;
            cur_enemy++;
        }
    }

    // Initialize pushable block
    init_block_sprite();
}

// Teleport player to the other teleporter location
void teleport() {
    // Clear current player position in level data
    level_data[objects[0].dx + objects[0].dy * LEVEL_WIDTH] = 0;

    // Find the other teleporter (tile type 6)
    int teleporter_found = 0;
    for (int c = 0; c < LEVEL_WIDTH * LEVEL_HEIGHT; c++) {
        if (level_data[c] == 6) {
            // Found the destination teleporter
            objects[0].dx = c % LEVEL_WIDTH;
            objects[0].dy = c / LEVEL_WIDTH;
            objects[0].x = (c % LEVEL_WIDTH) * 16 + 8;
            objects[0].y = (c / LEVEL_WIDTH) * 16 + 8;

            // Clear the destination teleporter as well
            level_data[c] = 0;

            teleporter_found = 1;
            ESP_LOGI("game", "Teleported to position (%d, %d)", objects[0].dx, objects[0].dy);
            break;
        }
    }

    if (!teleporter_found) {
        ESP_LOGW("game", "No destination teleporter found!");
    }
}

// Turn screen upside down (flip vertically) based on original game
void turn_screen() {
    ESP_LOGI("game", "Screen flip activated!");

    // Clear player position in level data
    level_data[objects[0].dx + objects[0].dy * LEVEL_WIDTH] = 0;

    // Flip the level data vertically
    for (int y = 0; y < LEVEL_HEIGHT / 2; y++) {
        for (int x = 0; x < LEVEL_WIDTH; x++) {
            int top_pos = y * LEVEL_WIDTH + x;
            int bottom_pos = (LEVEL_HEIGHT - 1 - y) * LEVEL_WIDTH + x;

            // Swap tiles
            int temp = level_data[top_pos];
            level_data[top_pos] = level_data[bottom_pos];
            level_data[bottom_pos] = temp;
        }
    }

    // Flip all objects (including player) vertically
    for (int c = 0; c < MAX_OBJECTS; c++) {
        if (objects[c].l) {
            objects[c].dy = LEVEL_HEIGHT - 1 - objects[c].dy;
            objects[c].y = (LEVEL_HEIGHT - 1) * 16 + 8 - (objects[c].y - 8);
        }
    }

    // Force level redraw
    reset_level_drawing();
    print_level();
}

// Search for rock at specific coordinates
int search_rock(int xr, int yr) {
    for (int c = 5; c < 15; c++) {
        if (objects[c].l && objects[c].dx == xr && objects[c].dy == yr) {
            return c;
        }
    }
    return -1; // No rock found
}

// Check if a tile type is passable for the player
bool is_passable(int tile_type) {
    switch (tile_type) {
        case 0: // Empty space
        case 1: // Small dot/pellet
        case 4: // Fruit
        case 5: // Bonus item
        case 6: // Teleporter
        case 7: // Time bonus
        case 8: // Screen flip item
        case 9: // Extra life
        case 10: // Freeze enemies item
        case 12: // Death trap (passable but deadly)
        case 81: // Temporary marker (from original - enemy/rock can pass)
            return true;
        case 2: // Wall
        case 3: // Rock/movable block (NOT passable - must be pushed)
        case 11: // Block (pushable)
        case 13: // Enemy (vertical)
        case 14: // Enemy (horizontal)
        case 80: // Special wall/rock state (from original)
        case 255: // Reserved space (rock being pushed)
        default:
            return false;
    }
}

// Handle item collection based on original game mechanics
void get_item() {
    int item = level_data[objects[0].dx + objects[0].dy * LEVEL_WIDTH];
    level_data[objects[0].dx + objects[0].dy * LEVEL_WIDTH] = 0;

    switch (item) {
        case 1: // Small dot/pellet
            score += 10;
            break;
        case 4: // Fruit
            fruit--;
            score += 500;
            ESP_LOGI("game", "Fruit collected! Remaining: %d, Score: %d", fruit, score);
            break;
        case 5: // Bonus item
            score += 100;
            ESP_LOGI("game", "Bonus collected! Score: %d", score);
            break;
        case 6: // Teleporter
            teleport();
            score += 200;
            break;
        case 7: // Time bonus
            av_time += 50; // Add extra time (reduced from original 500 for balance)
            ESP_LOGI("game", "Time bonus! Extra time: %d", av_time);
            break;
        case 8: // Screen flip
            turn_screen();
            score += 300;
            break;
        case 9: // Extra life
            lives++;
            ESP_LOGI("game", "Extra life! Lives: %d", lives);
            break;
        case 10: // Freeze enemies
            freeze_enemy = 300; // 5 seconds at 60fps (reduced from original 500)
            score += 150;
            ESP_LOGI("game", "Enemy freeze activated! Duration: %d frames", freeze_enemy);
            break;
        case 12: // Death trap
            dead = 1;
            ESP_LOGI("game", "Death trap hit!");
            break;
        default:
            // No item or unknown item - do nothing
            break;
    }
}

// Move rocks with gravity (from original game) - Fixed collision detection
void move_rocks() {
    for (int r = 5; r < 15; r++) {
        if (objects[r].l) {
            // Is there a rock?
            // Check if rock should fall (gravity) - only if not currently moving
            if (!objects[r].is_moving && objects[r].dy < LEVEL_HEIGHT - 1) {
                int below_pos = objects[r].dx + (objects[r].dy + 1) * LEVEL_WIDTH;
                int d = level_data[below_pos];

                if (d == 0 || d == 81) {
                    // Rock allowed to fall?
                    // Check if player is directly below (don't crush player immediately)
                    bool player_below = (objects[r].dx == objects[0].dx && objects[r].dy == objects[0].dy - 1);
                    if (!player_below) {
                        objects[r].movement_start_time = get_time_us();
                        objects[r].is_moving = true;
                        objects[r].dir = DOWN;
                        objects[r].l = 2; // Mark as falling
                        objects[r].start_x = objects[r].x;
                        objects[r].start_y = objects[r].y;
                        objects[r].target_dx = objects[r].dx;
                        objects[r].target_dy = objects[r].dy + 1;
                        objects[r].target_x = objects[r].dx * 16 + 8;
                        objects[r].target_y = (objects[r].dy + 1) * 16 + 8;
                        level_data[objects[r].dx + objects[r].dy * LEVEL_WIDTH] = 80; // Mark old pos as moving
                        ESP_LOGI("gravity", "Rock at (%d,%d) starting to fall", objects[r].dx, objects[r].dy);
                    }
                }
            }

            // Update rock position during movement
            if (objects[r].is_moving) {
                uint64_t current_time = get_time_us();
                uint64_t elapsed = current_time - objects[r].movement_start_time;

                if (elapsed >= TILE_MOVEMENT_DURATION_US) {
                    // Movement complete - check if we can continue falling or must stop
                    objects[r].is_moving = false;
                    objects[r].dir = 0;

                    // Clear old position (but check first to avoid clearing wrong tile)
                    int old_pos = objects[r].dx + objects[r].dy * LEVEL_WIDTH;
                    if (level_data[old_pos] == 80 || level_data[old_pos] == 255) {
                        level_data[old_pos] = 0; // Clear the moving marker
                    }

                    // Update to new position
                    objects[r].dx = objects[r].target_dx;
                    objects[r].dy = objects[r].target_dy;
                    objects[r].x = objects[r].target_x;
                    objects[r].y = objects[r].target_y;

                    // Place rock at new position
                    level_data[objects[r].dx + objects[r].dy * LEVEL_WIDTH] = 3;
                    objects[r].l = 1; // Rock is stationary again
                    ESP_LOGI("gravity", "Rock moved to (%d,%d)", objects[r].dx, objects[r].dy);

                    // Check if rock can continue falling (based on original logic)
                    if (objects[r].dy < LEVEL_HEIGHT - 1) {
                        int below_pos = objects[r].dx + (objects[r].dy + 1) * LEVEL_WIDTH;
                        int d = level_data[below_pos];
                        
                        // Only continue falling if destination is clear
                        if (d == 0 || d == 81) {
                            // Check if player is directly below (don't crush player immediately)
                            bool player_below = (objects[r].dx == objects[0].dx && objects[r].dy == objects[0].dy - 1);
                            if (!player_below) {
                                // Continue falling immediately - set up next fall
                                objects[r].movement_start_time = current_time; // Start next fall immediately
                                objects[r].is_moving = true;
                                objects[r].dir = DOWN;
                                objects[r].l = 2; // Mark as falling
                                objects[r].start_x = objects[r].x;
                                objects[r].start_y = objects[r].y;
                                objects[r].target_dx = objects[r].dx;
                                objects[r].target_dy = objects[r].dy + 1;
                                objects[r].target_x = objects[r].dx * 16 + 8;
                                objects[r].target_y = (objects[r].dy + 1) * 16 + 8;
                                level_data[objects[r].dx + objects[r].dy * LEVEL_WIDTH] = 80; // Mark old pos as moving
                                ESP_LOGI("gravity", "Rock continues falling from (%d,%d)", objects[r].dx, objects[r].dy);
                            }
                        } else {
                            ESP_LOGI("gravity", "Rock stopped at (%d,%d) - blocked by tile %d", objects[r].dx, objects[r].dy, d);
                        }
                    }
                } else {
                    // Interpolate position - DO NOT update target during movement!
                    float progress = (float) elapsed / TILE_MOVEMENT_DURATION_US;
                    if (progress > 1.0f) progress = 1.0f;

                    objects[r].x = objects[r].start_x + (objects[r].target_x - objects[r].start_x) * progress;
                    objects[r].y = objects[r].start_y + (objects[r].target_y - objects[r].start_y) * progress;
                    
                    // FIXED: Don't update target during interpolation - this was causing rocks to fall through blocks!
                    // The target should remain fixed during a single movement step
                }
            } else {
                objects[r].l = 1; // Ensure rock is marked as stationary
            }
        }
    }
}

// Move pushable stone block (object 15) - Sokoban-style movement
void move_block() {
    if (objects[15].l && objects[15].is_moving) {
        uint64_t current_time = get_time_us();
        uint64_t elapsed = current_time - objects[15].movement_start_time;

        if (elapsed >= TILE_MOVEMENT_DURATION_US) {
            // Movement complete - stone block becomes stationary tile
            objects[15].is_moving = false;
            objects[15].dir = 0;
            
            // Update final position (should already be set, but ensure consistency)
            objects[15].x = objects[15].target_x;
            objects[15].y = objects[15].target_y;
            
            // Deactivate object - the stone block is now handled by level tile 11
            objects[15].l = 0; // Deactivate object, level tile takes over
            
            // Ensure level data is set correctly at final position
            level_data[objects[15].dx + objects[15].dy * LEVEL_WIDTH] = 11;
            
            ESP_LOGI("stone_block", "Stone block movement completed at (%d,%d) - object deactivated, level tile active", 
                     objects[15].dx, objects[15].dy);
            
            // Force a full redraw to ensure the level tile is drawn
            full_redraw_needed = true;
        } else {
            // Interpolate position during movement
            float progress = (float) elapsed / TILE_MOVEMENT_DURATION_US;
            if (progress > 1.0f) progress = 1.0f;

            objects[15].x = objects[15].start_x + (objects[15].target_x - objects[15].start_x) * progress;
            objects[15].y = objects[15].start_y + (objects[15].target_y - objects[15].start_y) * progress;
        }
    }
}

// Render game objects with optimized rendering
void print_objects() {
    SDL_SetRenderTarget(renderer, game_surface);
    // Only render player object (index 0) for now to improve performance
    // Full game would require more complex dirty rectangle tracking
    if (objects[0].l) {
        SDL_FRect src_rect = {(float) objects[0].sx, (float) objects[0].sy, 16.0f, 16.0f};
        SDL_FRect dst_rect = {(float) objects[0].x, (float) objects[0].y, 16.0f, 16.0f};
        SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
    }

    // Render rocks (objects 5-14) with their current positions
    for (int nc = 5; nc < 15; nc++) {
        if (objects[nc].l) {
            SDL_FRect src_rect = {48.0f, 16.0f, 16.0f, 16.0f}; // Rock sprite
            SDL_FRect dst_rect = {(float) objects[nc].x, (float) objects[nc].y, 16.0f, 16.0f};
            SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
        }
    }

    // Render pushable block (object 15)
    if (objects[15].l) {
        SDL_FRect src_rect = {(float) (11 * 16), 16.0f, 16.0f, 16.0f}; // Block sprite
        SDL_FRect dst_rect = {(float) objects[15].x, (float) objects[15].y, 16.0f, 16.0f};
        SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
    }

    // Render enemies (objects 1-4) - simplified for now
    for (int nc = 1; nc < 5; nc++) {
        if (objects[nc].l) {
            SDL_FRect src_rect = {(float) objects[nc].sx, (float) objects[nc].sy, 16.0f, 16.0f};
            SDL_FRect dst_rect = {(float) objects[nc].x, (float) objects[nc].y, 16.0f, 16.0f};
            SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
        }
    }
}

// Tile-based player movement with continuous smooth sliding
void move_player() {
    // Always update position first
    update_player_position();

    // If still moving, don't start new movement
    if (objects[0].is_moving) {
        return;
    }

    // Handle escape key
    if (keyboard_state[SDL_SCANCODE_ESCAPE]) {
        dead = 1;
        return;
    }

    // Handle level navigation keys for testing
    static bool f2_pressed = false, f3_pressed = false;
    
    if (keyboard_state[SDL_SCANCODE_F2] && !f2_pressed) {
        // Previous level (F2) - only trigger once per press
        if (level > 1) {
            level--;
            level_change_requested = 1; // Set flag to indicate level change
            fruit = 0; // End current level
            ESP_LOGI("debug", "F2 pressed - moving to previous level %d", level);
        }
        f2_pressed = true;
        return;
    } else if (!keyboard_state[SDL_SCANCODE_F2]) {
        f2_pressed = false;
    }
    
    if (keyboard_state[SDL_SCANCODE_F3] && !f3_pressed) {
        // Next level (F3) - only trigger once per press
        if (level < 25) {
            level++;
            level_change_requested = 1; // Set flag to indicate level change
            fruit = 0; // End current level
            ESP_LOGI("debug", "F3 pressed - moving to next level %d", level);
        }
        f3_pressed = true;
        return;
    } else if (!keyboard_state[SDL_SCANCODE_F3]) {
        f3_pressed = false;
    }

    // Not currently moving - check for new movement input
    int target_dx = objects[0].dx;
    int target_dy = objects[0].dy;
    int direction = 0;
    int sprite_sy = objects[0].sy; // Keep current sprite direction

    // First check for accelerometer single moves (higher precision)
#ifdef CONFIG_FRUITLAND_ACCELEROMETER_INPUT
    int accel_move = accelerometer_get_pending_move();
    if (accel_move != 0) {
        // Process accelerometer move (dir_index + 1: LEFT=1, RIGHT=2, UP=3, DOWN=4)
        if (accel_move == 3 && objects[0].dy > 0) { // UP
            target_dy = objects[0].dy - 1;
            direction = UP;
            sprite_sy = 64;
        } else if (accel_move == 4 && objects[0].dy < LEVEL_HEIGHT - 1) { // DOWN
            target_dy = objects[0].dy + 1;
            direction = DOWN;
            sprite_sy = 80;
        } else if (accel_move == 1 && objects[0].dx > 0) { // LEFT
            target_dx = objects[0].dx - 1;
            direction = LEFT;
            sprite_sy = 32;
        } else if (accel_move == 2 && objects[0].dx < LEVEL_WIDTH - 1) { // RIGHT
            target_dx = objects[0].dx + 1;
            direction = RIGHT;
            sprite_sy = 48;
        }
        
        // Consume the move regardless of whether it was valid
        accelerometer_consume_pending_move();
    }
#endif

    // If no accelerometer move, check keyboard input
    if (direction == 0 && keyboard_state[SDL_SCANCODE_UP] && objects[0].dy > 0) {
        target_dy = objects[0].dy - 1;
        direction = UP;
        sprite_sy = 64; // Up-facing sprite
    } else if (keyboard_state[SDL_SCANCODE_DOWN] && objects[0].dy < LEVEL_HEIGHT - 1) {
        target_dy = objects[0].dy + 1;
        direction = DOWN;
        sprite_sy = 80; // Down-facing sprite
    } else if (keyboard_state[SDL_SCANCODE_LEFT] && objects[0].dx > 0) {
        target_dx = objects[0].dx - 1;
        direction = LEFT;
        sprite_sy = 32; // Left-facing sprite
    } else if (keyboard_state[SDL_SCANCODE_RIGHT] && objects[0].dx < LEVEL_WIDTH - 1) {
        target_dx = objects[0].dx + 1;
        direction = RIGHT;
        sprite_sy = 48; // Right-facing sprite
    }

    // If no valid direction, stay put
    if (direction == 0) {
        return;
    }

    // Check if target tile is passable or pushable
    int target_tile = level_data[target_dx + target_dy * LEVEL_WIDTH];

    // Handle stone block pushing (type 11 = stone block) - Sokoban-style like original
    if (target_tile == 11) {
        // Stone blocks are handled by object 15 and use simple Sokoban rules
        int push_target_dx = target_dx;
        int push_target_dy = target_dy;
        
        // Calculate push destination
        if (direction == LEFT) {
            if (target_dx <= 0) return; // At left edge
            push_target_dx = target_dx - 1;
        } else if (direction == RIGHT) {
            if (target_dx >= LEVEL_WIDTH - 1) return; // At right edge  
            push_target_dx = target_dx + 1;
        } else if (direction == UP) {
            if (target_dy <= 0) return; // At top edge
            push_target_dy = target_dy - 1;
        } else if (direction == DOWN) {
            if (target_dy >= LEVEL_HEIGHT - 1) return; // At bottom edge
            push_target_dy = target_dy + 1;
        }
        
        // Check if push destination is free (basic Sokoban rule)
        int destination_tile = level_data[push_target_dx + push_target_dy * LEVEL_WIDTH];
        if (destination_tile != 0) {
            ESP_LOGD("stone_push", "Stone block destination (%d,%d) blocked by tile %d", 
                     push_target_dx, push_target_dy, destination_tile);
            return; // Destination blocked
        }
        
        // Stone block push is allowed - set up object 15 movement
        ESP_LOGI("stone_push", "Pushing stone block %s from (%d,%d) to (%d,%d)",
                 (direction == LEFT) ? "left" : (direction == RIGHT) ? "right" :
                 (direction == UP) ? "up" : "down",
                 target_dx, target_dy, push_target_dx, push_target_dy);
        
        // Clear old stone block position
        level_data[target_dx + target_dy * LEVEL_WIDTH] = 0;
        
        // Set up object 15 (stone block) movement
        objects[15].l = 1; // Activate stone block object
        objects[15].dx = push_target_dx; // Set logical position  
        objects[15].dy = push_target_dy;
        objects[15].start_x = target_dx * 16 + 8; // Animation start position
        objects[15].start_y = target_dy * 16 + 8;
        objects[15].target_x = push_target_dx * 16 + 8; // Animation target
        objects[15].target_y = push_target_dy * 16 + 8;
        objects[15].x = objects[15].start_x; // Current position for animation
        objects[15].y = objects[15].start_y;
        objects[15].movement_start_time = get_time_us();
        objects[15].is_moving = true;
        objects[15].dir = direction;
        
        // Place stone block at destination in level data
        level_data[push_target_dx + push_target_dy * LEVEL_WIDTH] = 11;
        
        // Player can move into the stone block's old position
    } else if (target_tile == 3) {
        // Handle rock pushing (type 3 = rock) - simplified to match original behavior
        // Find the rock object at target position
        int rock_idx = search_rock(target_dx, target_dy);
        if (rock_idx < 0) {
            ESP_LOGD("push", "Rock not found at (%d,%d)", target_dx, target_dy);
            return;
        }

        // Calculate push destination based on direction (Sokoban-style)
        int push_target_dx = target_dx;
        int push_target_dy = target_dy;
        
        if (direction == LEFT) {
            if (target_dx <= 0) return; // At left edge, cannot push
            push_target_dx = target_dx - 1;
        } else if (direction == RIGHT) {
            if (target_dx >= LEVEL_WIDTH - 1) return; // At right edge, cannot push
            push_target_dx = target_dx + 1;
        } else if (direction == UP) {
            if (target_dy <= 0) return; // At top edge, cannot push
            push_target_dy = target_dy - 1;
        } else if (direction == DOWN) {
            // In original game, pushing rocks down was not allowed
            ESP_LOGD("push", "Cannot push rock down - not allowed in original");
            return;
        }

        // Check if push destination is free (basic Sokoban rule)
        int destination_tile = level_data[push_target_dx + push_target_dy * LEVEL_WIDTH];
        if (destination_tile != 0) {
            ESP_LOGD("push", "Push destination (%d,%d) blocked by tile %d", push_target_dx, push_target_dy, destination_tile);
            return; // Destination blocked, cannot push
        }

        // Additional stability check for LEFT/RIGHT pushes (from original)
        if (direction == LEFT || direction == RIGHT) {
            // Check if rock would be stable after push (has support below)
            if (push_target_dy < LEVEL_HEIGHT - 1) {
                int below_target = level_data[push_target_dx + (push_target_dy + 1) * LEVEL_WIDTH];
                if (below_target == 0) {
                    ESP_LOGD("push", "Rock would be unsupported after push - blocking");
                    return; // Rock would fall, don't allow push
                }
            }
        }

        // Push is allowed - set up rock movement (using time-based animation)
        ESP_LOGI("push", "Pushing rock %s from (%d,%d) to (%d,%d)",
                 (direction == LEFT) ? "left" : (direction == RIGHT) ? "right" :
                 (direction == UP) ? "up" : "down",
                 target_dx, target_dy, push_target_dx, push_target_dy);

        // Clear old rock position in level data
        level_data[target_dx + target_dy * LEVEL_WIDTH] = 0;
        
        // Set up rock movement animation
        objects[rock_idx].target_dx = push_target_dx;
        objects[rock_idx].target_dy = push_target_dy;
        objects[rock_idx].start_x = objects[rock_idx].x;
        objects[rock_idx].start_y = objects[rock_idx].y;
        objects[rock_idx].target_x = push_target_dx * 16 + 8;
        objects[rock_idx].target_y = push_target_dy * 16 + 8;
        objects[rock_idx].movement_start_time = get_time_us();
        objects[rock_idx].is_moving = true;
        objects[rock_idx].dir = direction;

        // Mark destination as reserved to prevent conflicts
        level_data[push_target_dx + push_target_dy * LEVEL_WIDTH] = 255;
        
        // Player can move into the rock's old position
    } else if (!is_passable(target_tile)) {
        ESP_LOGD("movement", "Target tile (%d, %d) blocked by tile type %d", target_dx, target_dy, target_tile);
        return;
    }

    // Start movement to target tile
    objects[0].target_dx = target_dx;
    objects[0].target_dy = target_dy;
    objects[0].start_x = objects[0].x;
    objects[0].start_y = objects[0].y;
    objects[0].target_x = target_dx * 16 + 8;
    objects[0].target_y = target_dy * 16 + 8;
    objects[0].movement_start_time = get_time_us();
    objects[0].is_moving = true;
    objects[0].dir = direction;

    // Initialize enhanced animation state
    objects[0].base_sy = sprite_sy; // Store base y-coordinate for this direction
    objects[0].current_frame = 0; // Start with first animation frame
    objects[0].last_anim_time = get_time_us(); // Reset animation timer
    objects[0].sx = 0; // Will be updated by animation system
    objects[0].sy = sprite_sy; // Will be updated by animation system

    ESP_LOGI("movement", "Starting movement from (%d, %d) to (%d, %d)",
             objects[0].dx, objects[0].dy, target_dx, target_dy);
}

// Initialize pushable stone block sprite coordinates (object 15)
void init_block_sprite() {
    // Reset object 15 (stone block handler)
    memset(&objects[15], 0, sizeof(objects[15]));
    objects[15].sx = 11 * 16; // Sprite x-coordinate (11th tile in patterns.bmp)
    objects[15].sy = 16;      // Sprite y-coordinate (second row)
    objects[15].l = 0;        // Initially not active
    objects[15].is_moving = false;
    objects[15].dir = 0;
    ESP_LOGD("init", "Stone block object 15 initialized and ready");
}

// Main game loop
int game() {
    level = 1;
    lives = 3;
    score = 0;

    while (level <= 25 && lives > 0) {
        reset_level_drawing(); // Reset level drawing flag for new level
        ESP_LOGI("game", "🎯 Starting Level %d (Lives: %d, Score: %d)", level, lives, score);
        init_level_data();
        print_level();
        count_fruit();
        init_objects();
        dead = 0;
        freeze_enemy = 0;
        level_change_requested = 0; // Reset level change flag for new level

        // Calculate scaling factor once for ESP32-P4 PPA optimization
        static float cached_scale = 0;
        static int cached_scaled_w = 0, cached_scaled_h = 0;
        static int cached_offset_x = 0, cached_offset_y = 0;
        (void) cached_offset_x; // Suppress unused warning
        (void) cached_offset_y; // Suppress unused warning

        if (cached_scale == 0) {
            float scale_x = (float) SCREEN_WIDTH / GAME_WIDTH;
            float scale_y = (float) SCREEN_HEIGHT / GAME_HEIGHT;
            cached_scale = (scale_x < scale_y) ? scale_x : scale_y;

            cached_scaled_w = GAME_WIDTH * cached_scale;
            cached_scaled_h = GAME_HEIGHT * cached_scale;
            cached_offset_x = (SCREEN_WIDTH - cached_scaled_w) / 2;
            cached_offset_y = (SCREEN_HEIGHT - cached_scaled_h) / 2;

#ifdef CONFIG_IDF_TARGET_ESP32P4
            // Temporarily disable PPA hardware scaling for debugging
            int scale_factor_int = (int) cached_scale;
            if (scale_factor_int > 1) {
                printf("PPA hardware scaling available but disabled for debugging: %dx\n", scale_factor_int);
                // TODO: Re-enable once buffer alignment issues are resolved
                // set_scale_factor(scale_factor_int, cached_scale);
            }
#endif
        }

        // Initialize performance tracking for new level
        last_frame_time = get_time_us();

        // Game loop for current level - optimized with dirty rectangles
        while (fruit > 0 && av_time > 0 && !dead && !level_change_requested) {
            uint64_t frame_start = get_time_us();

            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    return 0;
                }
            }

            // Process USB HID keyboard events (ESP32-P4 only)
#ifdef CONFIG_IDF_TARGET_ESP32P4
            if (is_keyboard_available()) {
                process_keyboard();
            }
#endif

            // Process accelerometer input events (reduced frequency for performance)
#ifdef CONFIG_FRUITLAND_ACCELEROMETER_INPUT
            static int accel_counter = 0;
            if (is_accelerometer_available() && (++accel_counter >= 4)) {
                process_accelerometer();
                accel_counter = 0;
            }
#endif

            keyboard_state = SDL_GetKeyboardState(NULL);

            // Store previous state for change detection
            static int prev_score = -1, prev_time = -1, prev_level = -1, prev_lives = -1;
            static int prev_player_x = -1, prev_player_y = -1;
            static int prev_rock_x[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
            static int prev_rock_y[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
            static int prev_block_x = -1, prev_block_y = -1; // Stone block position tracking
            static bool first_render = true;

            // Update game state
            move_player();
            move_rocks(); // Handle rock gravity and movement
            move_block(); // Handle pushable block movement

            // Decrease time every second (TARGET_FPS frames at target fps)
            static int time_counter = 0;
            if (++time_counter >= TARGET_FPS) {
                av_time--;
                time_counter = 0;
            }

            // Detect what changed for tile-based movement optimization
            bool player_moved = (objects[0].x != prev_player_x || objects[0].y != prev_player_y);
            bool stats_changed = (score != prev_score || av_time != prev_time ||
                                  level != prev_level || lives != prev_lives);

            // Check if any rocks moved
            bool rocks_moved = false;
            for (int r = 0; r < 10; r++) {
                // Check rocks 5-14 (index 0-9 in prev_rock arrays)
                int rock_idx = r + 5;
                if (objects[rock_idx].l &&
                    (objects[rock_idx].x != prev_rock_x[r] || objects[rock_idx].y != prev_rock_y[r])) {
                    rocks_moved = true;
                    break;
                }
            }

            // Check if stone block moved
            bool block_moved = (objects[15].l && 
                               (objects[15].x != prev_block_x || objects[15].y != prev_block_y));

            // Efficient rendering: only render when something actually changed
            bool should_render = player_moved || rocks_moved || block_moved || stats_changed || first_render || full_redraw_needed;

            // Skip rendering if nothing changed
            if (!should_render) {
                wait_for_frame_time();
                continue;
            }

            // Only render if absolutely necessary
            if (should_render) {
#ifdef CONFIG_IDF_TARGET_ESP32P4
                if (direct_framebuffer_mode) {
                    // Use fast direct framebuffer rendering
                    if (!fb_ready) {
                        wait_for_frame_time();
                        continue;
                    }

                    xSemaphoreTake(fb_mutex, portMAX_DELAY);

                    // Full level redraw only on first render
                    if (first_render || full_redraw_needed) {
                        fb_clear(rgb_to_rgb565(0, 0, 0));

                        // Draw all level tiles efficiently
                        for (int y = 0; y < LEVEL_HEIGHT; y++) {
                            for (int x = 0; x < LEVEL_WIDTH; x++) {
                                int tile = level_data[y * LEVEL_WIDTH + x];
                                if (tile != 0) {
                                    fb_draw_level_tile(x, y, tile);
                                }
                            }
                        }
                        full_redraw_needed = false;
                    }

                    // Clear previous positions if objects moved
                    if (player_moved && !first_render && prev_player_x >= 0) {
                        int tile_x = prev_player_x / 16;
                        int tile_y = (prev_player_y - 8) / 16;
                        if (tile_x >= 0 && tile_x < LEVEL_WIDTH && tile_y >= 0 && tile_y < LEVEL_HEIGHT) {
                            fb_draw_level_tile(tile_x, tile_y, level_data[tile_y * LEVEL_WIDTH + tile_x]);
                        }
                    }

                    // Clear previous rock positions
                    if (rocks_moved && !first_render) {
                        for (int r = 0; r < 10; r++) {
                            int rock_idx = r + 5;
                            if (prev_rock_x[r] >= 0 &&
                                (objects[rock_idx].l && (
                                     objects[rock_idx].x != prev_rock_x[r] || objects[rock_idx].y != prev_rock_y[r]))) {
                                int tile_x = prev_rock_x[r] / 16;
                                int tile_y = (prev_rock_y[r] - 8) / 16;
                                if (tile_x >= 0 && tile_x < LEVEL_WIDTH && tile_y >= 0 && tile_y < LEVEL_HEIGHT) {
                                    fb_draw_level_tile(tile_x, tile_y, level_data[tile_y * LEVEL_WIDTH + tile_x]);
                                }
                            }
                        }
                    }

                    // Draw player using fast direct framebuffer
                    if (objects[0].l) {
                        uint16_t player_color = rgb_to_rgb565(255, 255, 0); // Yellow
                        fb_draw_rect(objects[0].x, objects[0].y, 16, 16, player_color);
                    }

                    // Draw rocks using fast direct framebuffer
                    for (int r = 5; r < 15; r++) {
                        if (objects[r].l) {
                            uint16_t rock_color = rgb_to_rgb565(139, 69, 19); // Brown
                            fb_draw_rect(objects[r].x, objects[r].y, 16, 16, rock_color);

                            // Update cached position
                            prev_rock_x[r - 5] = objects[r].x;
                            prev_rock_y[r - 5] = objects[r].y;
                        } else {
                            prev_rock_x[r - 5] = -1;
                            prev_rock_y[r - 5] = -1;
                        }
                    }

                    // Draw stone block (object 15) using fast direct framebuffer
                    if (objects[15].l) {
                        uint16_t stone_color = rgb_to_rgb565(128, 128, 128); // Gray for stone block
                        fb_draw_rect(objects[15].x, objects[15].y, 16, 16, stone_color);
                        prev_block_x = objects[15].x;
                        prev_block_y = objects[15].y;
                    } else {
                        prev_block_x = -1;
                        prev_block_y = -1;
                    }

                    prev_player_x = objects[0].x;
                    prev_player_y = objects[0].y;

                    fb_ready = false;
                    xSemaphoreGive(fb_mutex);

                    // Trigger display update
                    fb_present();
                } else {
#endif
                // Optimized SDL rendering - redraw level once, then render objects
                if (first_render || full_redraw_needed) {
                    // Full level redraw only when necessary
                    clear_game_surface();
                    draw_border();
                    draw_level();
                    draw_texts();
                    full_redraw_needed = false;
                }

                // Efficient object rendering - draw all objects in one pass
                SDL_SetRenderTarget(renderer, game_surface);

                // Clear old object positions with black rectangles (fastest method)
                if (!first_render) {
                    if (player_moved && prev_player_x >= 0) {
                        SDL_FRect clear_rect = {prev_player_x, prev_player_y, 16, 16};
                        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                        SDL_RenderFillRect(renderer, &clear_rect);
                    }

                    if (rocks_moved) {
                        for (int r = 0; r < 10; r++) {
                            if (prev_rock_x[r] >= 0) {
                                SDL_FRect clear_rect = {prev_rock_x[r], prev_rock_y[r], 16, 16};
                                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                                SDL_RenderFillRect(renderer, &clear_rect);
                            }
                        }
                    }

                    if (block_moved && prev_block_x >= 0) {
                        SDL_FRect clear_rect = {prev_block_x, prev_block_y, 16, 16};
                        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
                        SDL_RenderFillRect(renderer, &clear_rect);
                    }
                }

                // Draw all active objects in one efficient pass
                // Player
                if (objects[0].l) {
                    SDL_FRect src_rect = {objects[0].sx, objects[0].sy, 16, 16};
                    SDL_FRect dst_rect = {objects[0].x, objects[0].y, 16, 16};
                    SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
                    prev_player_x = objects[0].x;
                    prev_player_y = objects[0].y;
                }

                // Rocks
                for (int r = 5; r < 15; r++) {
                    if (objects[r].l) {
                        SDL_FRect src_rect = {48, 16, 16, 16}; // Rock sprite
                        SDL_FRect dst_rect = {objects[r].x, objects[r].y, 16, 16};
                        SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
                        prev_rock_x[r - 5] = objects[r].x;
                        prev_rock_y[r - 5] = objects[r].y;
                    } else {
                        prev_rock_x[r - 5] = -1;
                        prev_rock_y[r - 5] = -1;
                    }
                }

                // Stone block (object 15)
                if (objects[15].l) {
                    SDL_FRect src_rect = {objects[15].sx, objects[15].sy, 16, 16}; // Stone block sprite (11*16, 16)
                    SDL_FRect dst_rect = {objects[15].x, objects[15].y, 16, 16};
                    SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
                    prev_block_x = objects[15].x;
                    prev_block_y = objects[15].y;
                } else {
                    prev_block_x = -1;
                    prev_block_y = -1;
                }

                // Handle stats changes
                if (stats_changed || first_render) {
                    print_stats();

                    prev_score = score;
                    prev_time = av_time;
                    prev_level = level;
                    prev_lives = lives;
                }
#ifdef CONFIG_IDF_TARGET_ESP32P4
                }
#endif

#ifdef CONFIG_IDF_TARGET_ESP32P4
                if (direct_framebuffer_mode) {
                    // Use direct framebuffer rendering for maximum performance
                    render_frame_direct_fb();
                    // fb_present() handles display update
                } else {
                    // Fallback to SDL rendering
                    render_frame_minimal();
                    SDL_RenderPresent(renderer);
                }
#else
                // Use minimal render function for better performance
                render_frame_minimal();
                SDL_RenderPresent(renderer);
#endif
                first_render = false;
            }

            // Track frame rendering time for performance monitoring
            uint64_t frame_end = get_time_us();
            uint64_t render_time = frame_end - frame_start;
            total_render_time += render_time;

            // Track performance statistics
            static uint64_t max_render_time = 0;
            static uint64_t min_render_time = UINT64_MAX;
            if (render_time > max_render_time) max_render_time = render_time;
            if (render_time < min_render_time && render_time > 0) min_render_time = render_time;

            // Log detailed performance every 10 seconds
            static uint64_t last_perf_log = 0;
            if (frame_end - last_perf_log >= 10000000) {
                // 10 seconds
                ESP_LOGI("PERF", "⚡ RENDER PERF: min=%llu us, max=%llu us, avg=%llu us",
                         min_render_time, max_render_time,
                         total_render_time / (frame_count > 0 ? frame_count : 1));
                ESP_LOGI("PERF", "🎯 EFFICIENCY: %.1f%% (render/budget ratio)",
                         (float)(total_render_time / (frame_count > 0 ? frame_count : 1)) / FRAME_TIME_US * 100.0f);

                last_perf_log = frame_end;
                // Reset min/max for next period
                max_render_time = 0;
                min_render_time = UINT64_MAX;
            }

            // Intelligent frame rate control
            wait_for_frame_time();
        }

        if (level_change_requested) {
            // F2/F3 level change - don't modify lives or score
            level_change_requested = 0; // Reset the flag
            ESP_LOGI("debug", "Level changed to %d via F2/F3", level);
        } else if (av_time == 0 || dead) {
            lives--;
        } else if (fruit == 0) {
            level++;
            score += av_time * 10;
        }
    }

    return 1;
}

void *sdl_thread(void *args) {
    printf("Fruit Land on ESP32\n");

    // Initialize filesystem first
    SDL_InitFS();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        printf("Unable to initialize SDL: %s\n", SDL_GetError());
        return NULL;
    }
    printf("SDL initialized successfully\n");

    // Set SDL3 performance hints optimized per target
#ifdef CONFIG_IDF_TARGET_ESP32P4
    // ESP32-P4 optimizations - leverage hardware capabilities
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0"); // Disable VSync for maximum performance
    SDL_SetHint("SDL_RENDER_SCALE_QUALITY", "1"); // Linear scaling (P4 can handle it)
    SDL_SetHint("SDL_RENDER_DRIVER", "software"); // Use software renderer (auto not supported)
    SDL_SetHint("SDL_FRAMEBUFFER_ACCELERATION", "1"); // Enable framebuffer acceleration
    SDL_SetHint("SDL_HINT_THREAD_PRIORITY_POLICY", "1"); // High thread priority
    SDL_SetHint("SDL_VIDEO_HIGHDPI_DISABLED", "1"); // Disable DPI scaling for compatibility
    SDL_SetHint("SDL_HINT_RENDER_BATCHING", "1"); // Enable render batching for efficiency
    printf("Applied ESP32-P4 software-accelerated optimizations\n");
#else
    // ESP32-S3 optimizations - conservative settings
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0"); // Disable VSync for maximum performance
    SDL_SetHint("SDL_RENDER_SCALE_QUALITY", "0"); // Use nearest-neighbor scaling (fastest)
    SDL_SetHint("SDL_RENDER_DRIVER", "software"); // Force software rendering for consistency
    SDL_SetHint("SDL_FRAMEBUFFER_ACCELERATION", "0"); // Disable framebuffer acceleration
    SDL_SetHint("SDL_HINT_THREAD_PRIORITY_POLICY", "1"); // High thread priority
    SDL_SetHint("SDL_VIDEO_HIGHDPI_DISABLED", "1"); // Disable DPI scaling
    SDL_SetHint("SDL_HINT_RENDER_BATCHING", "0"); // Disable render batching for immediate mode
    printf("Applied ESP32-S3 conservative optimizations\n");
#endif

#ifdef CONFIG_IDF_TARGET_ESP32P4
    // Initialize USB HID keyboard on ESP32-P4
    printf("Initializing USB HID keyboard...\n");
    esp_err_t keyboard_ret = init_keyboard();
    if (keyboard_ret == ESP_OK) {
        printf("USB HID keyboard initialized successfully\n");
    } else if (keyboard_ret != ESP_ERR_NOT_SUPPORTED) {
        printf("Warning: USB HID keyboard initialization failed: %s\n", esp_err_to_name(keyboard_ret));
    }
#endif

#ifdef CONFIG_FRUITLAND_ACCELEROMETER_INPUT
    // Initialize accelerometer input on supported boards
    printf("Initializing accelerometer input...\n");
    esp_err_t accel_ret = init_accelerometer();
    if (accel_ret == ESP_OK) {
        printf("Accelerometer input initialized successfully\n");
    } else if (accel_ret != ESP_ERR_NOT_SUPPORTED) {
        printf("Warning: Accelerometer input initialization failed: %s\n", esp_err_to_name(accel_ret));
    }
#endif

    // Get display dimensions
    const SDL_DisplayMode *display_mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (display_mode) {
        SCREEN_WIDTH = display_mode->w;
        SCREEN_HEIGHT = display_mode->h;
        printf("Display: %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    }

#ifdef CONFIG_IDF_TARGET_ESP32P4
    printf("ESP32-P4 detected - using optimized renderer settings\n");
#endif

    window = SDL_CreateWindow("Fruit Land", SCREEN_WIDTH, SCREEN_HEIGHT, 0);
    if (!window) {
        printf("Failed to create window: %s\n", SDL_GetError());
        SDL_Quit();
        return NULL;
    }

    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        printf("Failed to create renderer: %s\n", SDL_GetError());
        printf("Trying fallback renderer configuration...\n");

#ifdef CONFIG_IDF_TARGET_ESP32P4
        // Try more conservative settings for ESP32-P4
        SDL_SetHint("SDL_RENDER_SCALE_QUALITY", "0"); // Nearest neighbor scaling
        SDL_SetHint("SDL_FRAMEBUFFER_ACCELERATION", "0"); // Disable acceleration
        SDL_SetHint("SDL_HINT_RENDER_BATCHING", "0"); // Disable batching
        renderer = SDL_CreateRenderer(window, NULL);
#endif

        if (!renderer) {
            printf("Fallback renderer also failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(window);
            SDL_Quit();
            return NULL;
        } else {
            printf("Fallback renderer created successfully\n");
        }
    }

    if (!load_assets()) {
        printf("Failed to load game assets\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

    // Initialize render system with target-specific optimizations
#ifdef CONFIG_IDF_TARGET_ESP32P4
    printf("Using ESP32-P4 hardware-accelerated rendering\n");
    init_render_system();
#else
    printf("Using optimized single-core rendering\n");
    // Keep disabled for ESP32-S3 until stability issues are resolved
    // esp_err_t render_ret = init_render_system();
#endif

    printf("Starting game...\n");

    while (game_running) {
        show_intro();
        vTaskDelay(pdMS_TO_TICKS(2000)); // Show intro for 2 seconds
        game();
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause before restart
    }

    // Cleanup
#ifdef CONFIG_IDF_TARGET_ESP32P4
    cleanup_render_system(); // Cleanup hardware acceleration
    cleanup_keyboard();
#else
    // cleanup_render_system(); // Disabled with dual-core rendering
#endif
#ifdef CONFIG_FRUITLAND_ACCELEROMETER_INPUT
    cleanup_accelerometer();
#endif
    if (intro_texture) SDL_DestroyTexture(intro_texture);
    if (patterns_texture) SDL_DestroyTexture(patterns_texture);
    if (game_surface) SDL_DestroyTexture(game_surface);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return NULL;
}

void app_main(void) {
    // Note: Main task runs on Core 0 by default, rendering task will run on Core 1

    pthread_t sdl_pthread;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 65536); // Increased stack size for game

    int ret = pthread_create(&sdl_pthread, &attr, sdl_thread, NULL);
    if (ret != 0) {
        printf("Failed to create SDL thread: %d\n", ret);
        return;
    }

    pthread_detach(sdl_pthread);
}
