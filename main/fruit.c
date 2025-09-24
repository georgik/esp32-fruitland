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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "filesystem.h"
#include "keyboard.h"
#include "accelerometer.h"
#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "SDL3/SDL_esp-idf.h"  // For PPA hardware scaling
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
} OBJECT;

// Global game state
static SDL_Window *window = NULL;
static SDL_Renderer *renderer = NULL;
static SDL_Texture *intro_texture = NULL;
static SDL_Texture *patterns_texture = NULL;
static SDL_Texture *game_surface = NULL;

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
static int game_running = 1;

// Input state
static const bool *keyboard_state;

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

    // Load intro bitmap
    SDL_Surface *intro_surface = SDL_LoadBMP("/assets/intro.bmp");
    if (!intro_surface) {
        printf("Failed to load /assets/intro.bmp: %s\n", SDL_GetError());
        return 0;
    }
    intro_texture = SDL_CreateTextureFromSurface(renderer, intro_surface);
    SDL_DestroySurface(intro_surface);

    // Load patterns bitmap
    SDL_Surface *patterns_surface = SDL_LoadBMP("/assets/patterns.bmp");
    if (!patterns_surface) {
        printf("Failed to load /assets/patterns.bmp: %s\n", SDL_GetError());
        return 0;
    }
    patterns_texture = SDL_CreateTextureFromSurface(renderer, patterns_surface);
    SDL_DestroySurface(patterns_surface);

    // Create game surface texture for off-screen rendering
    game_surface = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
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
            SDL_FRect src_rect = {xx * 16, yy * 16 + 16, 16, 16};
            SDL_FRect dst_rect = {x * 16 + 8, y * 16 + 8, 16, 16};
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
    }
}

// Reset level drawing flag for new level
void reset_level_drawing() {
    level_drawn = false;
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
    level_data[c] = 0;

    // Initialize rocks and enemies (simplified)
    int cur_rock = 5;
    int cur_enemy = 1;

    for (c = 0; c < LEVEL_WIDTH * LEVEL_HEIGHT; c++) {
        if (level_data[c] == 3 && cur_rock < 15) {
            // Rock
            objects[cur_rock].dx = c % LEVEL_WIDTH;
            objects[cur_rock].dy = c / LEVEL_WIDTH;
            objects[cur_rock].x = (c % LEVEL_WIDTH) * 16 + 8;
            objects[cur_rock].y = (c / LEVEL_WIDTH) * 16 + 8;
            objects[cur_rock].l = 1;
            objects[cur_rock].sx = 48;
            objects[cur_rock].sy = 16;
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
}

// Render game objects with optimized rendering
void print_objects() {
    SDL_SetRenderTarget(renderer, game_surface);
    // Only render player object (index 0) for now to improve performance
    // Full game would require more complex dirty rectangle tracking
    if (objects[0].l) {
        SDL_FRect src_rect = {objects[0].sx, objects[0].sy, 16, 16};
        SDL_FRect dst_rect = {objects[0].x, objects[0].y, 16, 16};
        SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
    }

    // Render static objects (rocks, enemies) - these don't move often
    static bool static_objects_rendered = false;
    if (!static_objects_rendered) {
        for (int nc = 1; nc < MAX_OBJECTS; nc++) {
            if (objects[nc].l) {
                SDL_FRect src_rect = {objects[nc].sx, objects[nc].sy, 16, 16};
                SDL_FRect dst_rect = {objects[nc].x, objects[nc].y, 16, 16};
                SDL_RenderTexture(renderer, patterns_texture, &src_rect, &dst_rect);
            }
        }
        static_objects_rendered = true;
    }
}

// Simple player movement (basic implementation)
void move_player() {
    if (objects[0].step > 0) {
        objects[0].step--;

        // Animate sprite frames (alternate between frames on odd steps)
        if (objects[0].step & 1) {
            objects[0].sx = 16; // Second animation frame
        } else {
            objects[0].sx = 0; // First animation frame
        }

        // Continue movement animation
        if (objects[0].dir == UP) objects[0].y--;
        else if (objects[0].dir == DOWN) objects[0].y++;
        else if (objects[0].dir == LEFT) objects[0].x--;
        else if (objects[0].dir == RIGHT) objects[0].x++;

        if (objects[0].step == 0) {
            // Movement completed, update grid position
            if (objects[0].dir == UP) objects[0].dy--;
            else if (objects[0].dir == DOWN) objects[0].dy++;
            else if (objects[0].dir == LEFT) objects[0].dx--;
            else if (objects[0].dir == RIGHT) objects[0].dx++;
            objects[0].dir = 0;

            // Check for item pickup
            int item = level_data[objects[0].dx + objects[0].dy * LEVEL_WIDTH];
            if (item == 4) {
                // Fruit
                fruit--;
                score += 500;
                level_data[objects[0].dx + objects[0].dy * LEVEL_WIDTH] = 0;
            }
        }
        return;
    }

    // Start new movement
    if (keyboard_state[SDL_SCANCODE_UP] && objects[0].dy > 0) {
        int target = level_data[objects[0].dx + (objects[0].dy - 1) * LEVEL_WIDTH];
        if (target == 0 || target == 4) {
            // Empty or fruit
            objects[0].dir = UP;
            objects[0].step = 16;
            objects[0].sx = 0; // Fixed: start at sprite 0, not -16
            objects[0].sy = 64;
        }
    } else if (keyboard_state[SDL_SCANCODE_DOWN] && objects[0].dy < LEVEL_HEIGHT - 1) {
        int target = level_data[objects[0].dx + (objects[0].dy + 1) * LEVEL_WIDTH];
        if (target == 0 || target == 4) {
            objects[0].dir = DOWN;
            objects[0].step = 16;
            objects[0].sx = 0; // Fixed: start at sprite 0, not -16
            objects[0].sy = 80;
        }
    } else if (keyboard_state[SDL_SCANCODE_LEFT] && objects[0].dx > 0) {
        int target = level_data[objects[0].dx - 1 + objects[0].dy * LEVEL_WIDTH];
        if (target == 0 || target == 4) {
            objects[0].dir = LEFT;
            objects[0].step = 16;
            objects[0].sx = 0; // Fixed: start at sprite 0, not -16
            objects[0].sy = 32;
        }
    } else if (keyboard_state[SDL_SCANCODE_RIGHT] && objects[0].dx < LEVEL_WIDTH - 1) {
        int target = level_data[objects[0].dx + 1 + objects[0].dy * LEVEL_WIDTH];
        if (target == 0 || target == 4) {
            objects[0].dir = RIGHT;
            objects[0].step = 16;
            objects[0].sx = 0; // Fixed: start at sprite 0, not -16
            objects[0].sy = 48;
        }
    }

    if (keyboard_state[SDL_SCANCODE_ESCAPE]) {
        dead = 1;
    }
}

// Main game loop
int game() {
    level = 1;
    lives = 3;
    score = 0;

    while (level <= 25 && lives > 0) {
        reset_level_drawing(); // Reset level drawing flag for new level
        init_level_data();
        print_level();
        count_fruit();
        init_objects();
        dead = 0;
        freeze_enemy = 0;

        // Calculate scaling factor once for ESP32-P4 PPA optimization
        static float cached_scale = 0;
        static int cached_scaled_w = 0, cached_scaled_h = 0;
        static int cached_offset_x = 0, cached_offset_y = 0;

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

        // Game loop for current level
        while (fruit > 0 && av_time > 0 && !dead) {
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

            // Process accelerometer input events
#ifdef CONFIG_FRUITLAND_ACCELEROMETER_INPUT
            if (is_accelerometer_available()) {
                process_accelerometer();
            }
#endif

            keyboard_state = SDL_GetKeyboardState(NULL);

            // Only clear player's old position (more efficient)
            SDL_SetRenderTarget(renderer, game_surface);
            SDL_FRect clear_rect = {objects[0].x, objects[0].y, 16, 16};
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, &clear_rect);

            move_player();
            print_objects();
            av_time--;

            // Update statistics less frequently to improve performance
            static int stats_counter = 0;
            if (++stats_counter >= 10) {
                // Update stats every 10 frames
                print_stats();
                stats_counter = 0;
            }

            // Render game surface to screen with cached scaling
            SDL_SetRenderTarget(renderer, NULL);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);

            SDL_FRect dst_rect = {cached_offset_x, cached_offset_y, cached_scaled_w, cached_scaled_h};
            SDL_RenderTexture(renderer, game_surface, NULL, &dst_rect);
            SDL_RenderPresent(renderer);

            // Improved frame rate: ~60 FPS instead of 30 FPS
            vTaskDelay(pdMS_TO_TICKS(16)); // ~60 FPS
        }

        if (av_time == 0 || dead) {
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

    window = SDL_CreateWindow("Fruit Land", SCREEN_WIDTH, SCREEN_HEIGHT, 0);
    if (!window) {
        printf("Failed to create window: %s\n", SDL_GetError());
        SDL_Quit();
        return NULL;
    }

    renderer = SDL_CreateRenderer(window, NULL);
    if (!renderer) {
        printf("Failed to create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

    if (!load_assets()) {
        printf("Failed to load game assets\n");
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }

    printf("Starting game...\n");

    while (game_running) {
        show_intro();
        vTaskDelay(pdMS_TO_TICKS(2000)); // Show intro for 2 seconds
        game();
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause before restart
    }

    // Cleanup
#ifdef CONFIG_IDF_TARGET_ESP32P4
    cleanup_keyboard();
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
