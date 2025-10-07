cmake_minimum_required(VERSION 3.16)

# ESP-BSP SDL Universal Board Selection
# Board selection is now handled through KConfig (idf.py menuconfig)
# Navigate to "ESP-BSP SDL Configuration" -> "Select Target Board"
# Available boards: ESP-Box-3, M5Stack Core S3, M5 Atom S3, and more
#
# Usage:
#   idf.py menuconfig  # Configure board selection
#   idf.py build       # Build with selected board

# KConfig-based board configuration
# Board selection is handled entirely through SDKCONFIG_DEFAULTS and menuconfig
# No automatic file copying or board tracking - use:
# SDKCONFIG_DEFAULTS=sdkconfig.defaults.<board> idf.py reconfigure

# Board configuration detection for informational messages
# NOTE: IDF_TARGET is set by 'idf.py set-target' command, not here
if(CONFIG_SDL_BSP_ESP32_P4_FUNCTION_EV)
    message(STATUS "Detected ESP32-P4 Function EV Board configuration")
elseif(CONFIG_SDL_BSP_M5STACK_TAB5)
    message(STATUS "Detected M5Stack Tab5 (ESP32-P4) configuration")
elseif(CONFIG_SDL_BSP_ESP32_C6_DEVKIT)
    message(STATUS "Detected ESP32-C6 DevKit configuration")
elseif(CONFIG_SDL_BSP_ESP32_C3_LCDKIT)
    message(STATUS "Detected ESP32-C3 LCDkit configuration")
elseif(CONFIG_SDL_BSP_ESP_BOX_3)
    message(STATUS "Detected ESP32-S3-BOX-3 configuration")
elseif(CONFIG_SDL_BSP_ESP_BOX)
    message(STATUS "Detected ESP-Box configuration")
elseif(CONFIG_SDL_BSP_M5STACK_CORE_S3)
    message(STATUS "Detected M5Stack Core S3 configuration")
elseif(CONFIG_SDL_BSP_M5_ATOM_S3)
    message(STATUS "Detected M5 Atom S3 configuration")
else()
    message(STATUS "Board configuration will be detected at runtime")
endif()

set(COMPONENTS
    main
) # "Trim" the build. Include the minimal set of components; main and anything it depends on.
# Note: Both SDL and sdl_bsp are now managed as dependencies in main/idf_component.yml

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(fruitland)

# Create LittleFS partition image from assets folder
littlefs_create_partition_image(assets assets FLASH_IN_PROJECT)

# ==============================================================================
# Code Formatting with clang-format (ESP-IDF compatible)
# ==============================================================================

# Find clang-format executable
find_program(CLANG_FORMAT_EXECUTABLE clang-format)

if(CLANG_FORMAT_EXECUTABLE)
    message(STATUS "Found clang-format: ${CLANG_FORMAT_EXECUTABLE}")
    
    # Collect all C/C++ source files
    file(GLOB_RECURSE ALL_SOURCE_FILES 
        ${CMAKE_SOURCE_DIR}/main/*.c
        ${CMAKE_SOURCE_DIR}/main/*.cpp
        ${CMAKE_SOURCE_DIR}/main/*.h
        ${CMAKE_SOURCE_DIR}/main/*.hpp
    )
    
    # Filter out any files we don't want to format
    list(FILTER ALL_SOURCE_FILES EXCLUDE REGEX ".*/build/.*")
    
    # Custom target to check formatting
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${ALL_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking code formatting with clang-format"
        VERBATIM
    )
    
    # Custom target to apply formatting
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${ALL_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Formatting code with clang-format"
        VERBATIM
    )
    
    # Custom target to show formatting diff
    add_custom_target(format-diff
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Showing formatting differences"
        VERBATIM
    )
    
    # Add individual file diff commands
    foreach(SOURCE_FILE ${ALL_SOURCE_FILES})
        add_custom_command(TARGET format-diff POST_BUILD
            COMMAND echo "=== ${SOURCE_FILE} ==="
            COMMAND ${CLANG_FORMAT_EXECUTABLE} ${SOURCE_FILE} | diff -u ${SOURCE_FILE} - || true
            VERBATIM
        )
    endforeach()
    
    message(STATUS "Code formatting targets added:")
    message(STATUS "  idf.py format-check  - Check if files need formatting")
    message(STATUS "  idf.py format        - Apply formatting to all files")
    message(STATUS "  idf.py format-diff   - Show formatting differences")
    
else()
    message(WARNING "clang-format not found. Install with: brew install clang-format")
    
    # Add dummy targets so the build doesn't fail
    add_custom_target(format-check
        COMMAND echo "clang-format not available. Install with: brew install clang-format"
        COMMENT "clang-format not available"
    )
    
    add_custom_target(format
        COMMAND echo "clang-format not available. Install with: brew install clang-format"
        COMMENT "clang-format not available"
    )
    
    add_custom_target(format-diff
        COMMAND echo "clang-format not available. Install with: brew install clang-format"
        COMMENT "clang-format not available"
    )
endif()
