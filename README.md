# 🍎 Fruitland Game - SDL3 Port for ESP32

**Classic arcade-style fruit collection game** ported from Allegro to SDL3 for embedded systems.

🎯 **Cross-Platform** - The same SDL3 code runs on different ESP32 boards with various display sizes.

## 🎮 What This Demonstrates

- **🍎 Classic Game Port** - Port of old Allegro game to SDL3 conversion with game logic preservation
- **📱 Resolution Adaptation** - Automatically scales to different display sizes  
- **💾 LittleFS Integration** - Asset storage using embedded filesystem
- **🎛️ Board Abstraction** - Shows how SDL works across different ESP32 boards
- **🧠 PSRAM Utilization** - Heap allocation for large framebuffers and game assets

## 🚀 Quick Start

### 🌐 Web-based Flashing (Easiest)

Flash ESP32-Fruitland directly from your browser - no software installation required!

[![Try it with ESP Launchpad](https://espressif.github.io/esp-launchpad/assets/try_with_launchpad.png)](https://georgik.github.io/esp32-fruitland/?flashConfigURL=https://georgik.github.io/esp32-fruitland/config/config.toml)

**Requirements**: Chrome/Edge browser + ESP32 board with PSRAM + USB cable

### 🔧 Build from Source

**Prerequisites**:
- **PSRAM Required**: This game requires PSRAM for asset storage and framebuffers
- **LittleFS Assets**: Game assets are stored in LittleFS partition for efficient access

**Build and Flash**:
```bash
# Configure for ESP32-S3-BOX (recommended - has PSRAM)
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp-box-3" build flash monitor

# Or configure for M5Stack CoreS3 (also has PSRAM)
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.m5stack_core_s3" build flash monitor
```

### Game Description

**Fruitland** is a classic arcade-style game where players collect falling fruits while avoiding obstacles. This port maintains the original game mechanics while adapting to embedded hardware constraints:

- **🍎 Fruit Collection**: Catch different types of fruits for points
- **🎯 Obstacle Avoidance**: Navigate around falling hazards
- **🏆 Score System**: Classic arcade scoring with high score tracking
- **🎮 Responsive Controls**: Touch, button, or USB keyboard input depending on board capabilities
- **⌨️ USB Keyboard Support**: Full keyboard input on ESP32-P4 boards with USB host capability

### Alternative: Using ESPBrew
```bash
# Multi-board build manager with TUI (PSRAM boards only)
espbrew # Interactive mode - select boards and press 'b'
espbrew --cli-only  # Automatic mode - builds all detected PSRAM boards
```

**ESPBrew** automatically detects board configurations, but only PSRAM-enabled boards will work:
- **Auto-Discovery** - Finds `sdkconfig.defaults.*` files for PSRAM boards
- **TUI Interface** - Interactive selection and real-time build monitoring  
- **Parallel Builds** - Build multiple compatible boards simultaneously
- **Project**: https://github.com/georgik/espbrew

![Architecture Overview](docs/architecture.png)

## 🚀 Quick Start

### PSRAM-Compatible Board Selection

⚠️ **IMPORTANT**: Fruitland requires PSRAM for asset storage and framebuffers. Only use boards with PSRAM support.

```bash
# ESP32-S3-BOX-3 (320×240, OCTAL PSRAM) - RECOMMENDED
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp-box-3" build flash monitor

# M5Stack CoreS3 (320×240, QUAD PSRAM) - COMPATIBLE
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.m5stack_core_s3" build flash monitor

# M5Stack Tab5 (1280×720, 32MB PSRAM, ESP32-P4) - COMPATIBLE
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.m5stack_tab5" build flash monitor

# ESP32-P4 RISC-V (up to 1280×800, 32MB PSRAM) - COMPATIBLE
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp32_p4_function_ev_board" build flash monitor
```

### ❌ Incompatible Boards (No PSRAM)

These boards lack sufficient memory for the game:
```bash
# M5 Atom S3 (128×128, No PSRAM) - WILL NOT WORK
# ESP BSP Generic (Configurable, typically no PSRAM) - WILL NOT WORK  
# ESP BSP DevKit (Virtual display, no PSRAM) - WILL NOT WORK
```

### First Time Setup (Recommended Board)

```bash
# Use ESP32-S3-BOX-3 as the recommended default (has PSRAM)
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp-box-3" build flash monitor
```

## 🎛️ Supported Boards (PSRAM Required)

⚠️ **PSRAM Requirement**: Fruitland requires PSRAM for asset storage and framebuffers.

### ✅ Compatible Boards (PSRAM Available)

| Board | Architecture | Display | PSRAM | Input Support | Fruitland Status |
|-------|-------------|---------|-------|---------------|------------------|
| `esp_box_3` | ESP32-S3 (Xtensa) | 320×240 ILI9341 | OCTAL | Touch | 🍎 **RECOMMENDED** |
| `m5stack_core_s3` | ESP32-S3 (Xtensa) | 320×240 ILI9341 | QUAD | Touch | ✅ **Compatible** |
| `m5stack_tab5` | **ESP32-P4 (RISC-V)** | 1280×720 IPS via MIPI-DSI | 32MB | **⌨️ USB Keyboard + Touch** | ✅ **Compatible** |
| `esp32_p4_function_ev` | **ESP32-P4 (RISC-V)** | up to 1280×800 | 32MB | **⌨️ USB Keyboard + Touch** | ✅ **Compatible** |
| `esp32_s3_lcd_ev` | ESP32-S3 (Xtensa) | Multiple LCD types | 16MB OCTAL | Touch | ✅ **Compatible** |
| `esp32_s3_eye` | ESP32-S3 (Xtensa) | 240×240 circular | 8MB OCTAL | Touch | ✅ **Compatible** |
| `esp32_s3_korvo_2` | ESP32-S3 (Xtensa) | LCD + Camera | OCTAL | Touch | ✅ **Compatible** |

### ❌ Incompatible Boards (No PSRAM)

| Board | Architecture | Display | PSRAM | Fruitland Status |
|-------|-------------|---------|-------|------------------|
| `m5_atom_s3` | ESP32-S3 (Xtensa) | 128×128 GC9A01 | None | ❌ **Insufficient Memory** |
| **`esp_bsp_generic`** | **Any ESP32** | **Configurable** | **Usually None** | ❌ **Typically No PSRAM** |
| `esp_bsp_devkit` | Any ESP32 | Virtual 240×320 | Configurable | ❌ **Usually No PSRAM** |

## 💾 LittleFS Asset Storage

**Fruitland** uses LittleFS for efficient asset storage on embedded systems:

### ✨ Features
- **🍎 Game Assets**: Stores intro screen, sprite patterns, and game data
- **🚀 Fast Access**: Optimized for embedded flash storage
- **💾 Small Footprint**: Minimal overhead compared to traditional filesystems
- **🔒 Reliable**: Power-loss resilient with wear leveling

### 💾 Asset Files Included
- **`intro.bmp`**: Game introduction screen background
- **`patterns.bmp`**: Sprite patterns for fruits and game objects  
- **`fruit.dat`**: Game data including level configurations

Assets are automatically packed into the LittleFS partition during the build process and mounted at `/assets/` at runtime.

## ⌨️ USB Keyboard Support (ESP32-P4 Only)

**Fruitland** includes full USB keyboard support for ESP32-P4 boards with USB host capability:

### 🎮 Keyboard Controls
- **Arrow Keys**: Move player character
- **WASD Keys**: Alternative movement controls
- **Space**: Action/Select
- **Enter**: Pause/Menu
- **ESC**: Quit game
- **Function Keys**: Debug features (F1-F12)

### 🔌 Hardware Requirements
- **ESP32-P4 Board**: M5Stack Tab5, ESP32-P4 Function EV Board
- **USB Keyboard**: Any standard USB keyboard (wired or wireless with USB receiver)
- **USB Host Port**: Connected to the board's USB host connector

### ⚡ Features
- **Real-time Input**: Event-driven keyboard handling with minimal latency
- **Auto-detection**: Automatic keyboard detection and initialization
- **Multi-key Support**: Simultaneous key press detection
- **SDL Integration**: Seamless translation of USB HID events to SDL keyboard events

> **Note**: Keyboard support is automatically enabled on ESP32-P4 boards. For other boards, standard touch/button controls remain available.

## 🏢 Architecture Highlights

### 🎯 Game Engine Architecture
- **SDL3 Rendering**: Hardware-accelerated graphics with board abstraction
- **Allegro to SDL3 Port**: Complete API translation maintaining game logic
- **PSRAM Utilization**: Large framebuffers and assets stored in external memory
- **LittleFS Integration**: Embedded filesystem for reliable asset storage

### 🧠 Memory Management
- **Heap Allocation**: Game assets and framebuffers use PSRAM heap allocation
- **Asset Caching**: Efficient loading and caching of game sprites and data  
- **Stack Optimization**: Minimal stack usage to prevent overflow on embedded systems
- **Memory Safety**: Proper cleanup and resource management throughout game lifecycle

### 🔧 Multi-Architecture Support
- **Xtensa (ESP32-S3)**: All ESP32-S3 based development boards with PSRAM
- **RISC-V (ESP32-P4)**: Next-gen ESP32-P4 with advanced multimedia features
- **Resolution Scaling**: Game adapts to different display sizes automatically

## 🚀 Advanced Usage

### Board Switching
```bash
# Switch between M5 Atom S3 and ESP32-S3-BOX-3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.m5_atom_s3" build flash monitor
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp-box-3" build flash monitor

# Switch to M5Stack Tab5
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.m5stack_tab5" build flash monitor

# Switch to other boards
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp32_p4_function_ev_board" build flash monitor

# Clean switch if needed (recommended when switching between different boards)
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.m5_atom_s3" fullclean build flash monitor
```

### Custom Board Development
```bash
# Use ESP BSP Generic as starting point
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp_bsp_generic" build
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp_bsp_generic" menuconfig  # Customize hardware configuration
# Save configuration for reuse
cp sdkconfig sdkconfig.defaults.my_custom_board
```

### IDE Integration

**VS Code ESP-IDF Extension:**
```json
{
    "idf.cmakeCompilerArgs": ["-D", "SDKCONFIG_DEFAULTS=sdkconfig.defaults.m5stack_core_s3"]
}
```

**CLion:** Project Settings → CMake → CMake options: `-D SDKCONFIG_DEFAULTS=sdkconfig.defaults.m5stack_core_s3`
