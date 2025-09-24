# ESP32-Fruitland Flash Instructions

Welcome to ESP32-Fruitland! This guide will help you flash the game to your ESP32 board.

## 🚀 Web-based Flashing (Recommended)

The easiest way to install ESP32-Fruitland is using our web-based installer:

[![Try it with ESP Launchpad](https://espressif.github.io/esp-launchpad/assets/try_with_launchpad.png)](https://georgik.github.io/esp32-fruitland/?flashConfigURL=https://georgik.github.io/esp32-fruitland/config/config.toml)

### Requirements
- **Chrome or Edge browser** (for WebUSB support)
- **ESP32 board with PSRAM** (see supported boards below)
- **USB cable** to connect your board

### Steps
1. Connect your ESP32 board to your computer via USB
2. Click the "Try it with ESP Launchpad" button above
3. Click "Connect" and select your board's serial port
4. Choose "ESP32-Fruitland" from the application list
5. Select your specific board model
6. Click "Flash" and wait for completion
7. The game will start automatically after flashing!

## 🛠️ Supported Hardware

**⚠️ IMPORTANT**: ESP32-Fruitland requires PSRAM for proper operation. Make sure your board has PSRAM enabled.

### ✅ Compatible Boards

| Board | Display | PSRAM | Status |
|-------|---------|-------|---------|
| **ESP32-S3-BOX-3** | 320×240 ILI9341 | OCTAL | 🍎 **Recommended** |
| **M5Stack CoreS3** | 320×240 ILI9341 | QUAD | ✅ Compatible |
| **ESP32-S3-LCD-EV-Board** | Multiple LCDs | 16MB OCTAL | ✅ Compatible |
| **ESP32-P4 Function EV Board** | Up to 1280×800 | 32MB | ✅ Compatible |
| **M5Stack Tab5** | 1280×720 MIPI-DSI | 32MB | ✅ Compatible |

### ❌ Incompatible Boards

Boards without PSRAM **will not work**:
- M5 Atom S3 (128×128, No PSRAM)
- ESP32-C3 DevKit (No PSRAM) 
- Basic ESP32 DevKits (Usually no PSRAM)

## 🎮 Game Features

ESP32-Fruitland brings classic arcade gaming to your ESP32:

### Core Gameplay
- **🍎 Fruit Collection**: Catch falling fruits for points
- **🎯 Obstacle Avoidance**: Navigate around hazards
- **🏆 Score System**: Classic arcade scoring
- **⚡ Power-ups**: Special items for bonus points

### Controls
- **Touch Screens**: Tap and drag to move
- **Button Controls**: Use directional buttons on your board
- **Keyboard Support**: Connect USB keyboard to ESP32-P4 boards

### Technical Features
- **SDL3 Graphics**: Smooth hardware-accelerated rendering
- **Multi-Resolution**: Automatically adapts to your display
- **LittleFS Storage**: Efficient game asset management
- **PSRAM Optimization**: Large framebuffers in external memory

## 🔧 Manual Installation (Advanced Users)

If you prefer manual installation:

### Prerequisites
- Python with esptool.py: `pip install esptool`
- ESP-IDF for building from source

### Download Firmware
1. Go to the [latest release](https://github.com/georgik/esp32-fruitland/releases/latest)
2. Download the `.bin` file for your board
3. Use esptool.py to flash:

```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 esp32-fruitland-v1.0.0-esp-box-3.bin
```

### Building from Source
```bash
git clone https://github.com/georgik/esp32-fruitland
cd esp32-fruitland
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults.esp-box-3" build flash monitor
```

## 🚨 Troubleshooting

### Flashing Issues
- **Serial Port Not Found**: Make sure your board is connected and drivers are installed
- **Permission Denied**: On Linux/macOS, add your user to the dialout group
- **Flash Fails**: Try lowering baud rate to 115200

### Hardware Issues
- **Game Won't Start**: Verify your board has PSRAM enabled
- **Display Issues**: Check your board configuration matches the sdkconfig
- **Touch Not Working**: Ensure touch driver is properly configured

### Browser Issues (Web Flashing)
- **WebUSB Not Supported**: Use Chrome or Edge browser
- **Connection Fails**: Try different USB ports/cables
- **Slow Flashing**: USB 2.0 ports may be slower than USB 3.0

## 🆘 Getting Help

If you encounter issues:

1. **Check Hardware**: Ensure your board has PSRAM and is supported
2. **Verify Connection**: Test with a simple blink example first  
3. **Update Drivers**: Make sure USB-to-serial drivers are current
4. **Try Different Browser**: Chrome and Edge work best for web flashing
5. **Check Issues**: Look at [GitHub Issues](https://github.com/georgik/esp32-fruitland/issues) for similar problems

## 📝 Technical Details

### Memory Requirements
- **PSRAM**: Required for framebuffers and game assets
- **Flash**: ~2MB for firmware, ~1MB for game assets
- **RAM**: ~200KB for game logic and buffers

### Partition Layout
```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
factory,  app,  factory, 0x10000,  0x100000,
storage,  data, spiffs,  0x110000, 0x100000,
```

### Performance Tips
- Use boards with OCTAL PSRAM for best performance
- Ensure adequate power supply (some boards need external power for displays)
- Higher resolution displays require more processing power

Enjoy playing ESP32-Fruitland! 🍎🎮