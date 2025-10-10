/**
 * @file config.h
 * @brief HotPin ESP32-CAM Hardware Configuration
 * 
 * Pin assignments, I2S configurations, and system constants for the
 * ESP32-CAM AI-Thinker module with dynamic camera/voice switching.
 * 
 * CRITICAL: This configuration assumes:
 * - ESP32-WROVER-E module with 4MB PSRAM
 * - SD card functionality completely disabled
 * - GPIOs 2, 4, 5, 12, 13, 14, 15 freed for audio use
 */

#ifndef CONFIG_H
#define CONFIG_H

#include "driver/gpio.h"
#include "driver/i2s.h"

/*******************************************************************************
 * SYSTEM CONFIGURATION
 ******************************************************************************/

// Target CPU frequency
#define CONFIG_CPU_FREQ_MHZ                 240

// FreeRTOS Task Stack Sizes
#define TASK_STACK_SIZE_LARGE               8192
#define TASK_STACK_SIZE_MEDIUM              4096
#define TASK_STACK_SIZE_SMALL               2048

// Task priorities helper
#define TASK_PRIORITY_BUTTON_HANDLER        5       // Button handling task

// PSRAM Configuration (MANDATORY)
#define CONFIG_PSRAM_REQUIRED               1
#define CONFIG_PSRAM_MIN_SIZE_MB            4

/*******************************************************************************
 * GPIO PIN ASSIGNMENTS (Table 3 from Architecture Document)
 ******************************************************************************/

// ========== User Input ==========
#define CONFIG_PUSH_BUTTON_GPIO             GPIO_NUM_4      // Mode switch/Shutdown
#define CONFIG_STATUS_LED_GPIO              GPIO_NUM_33     // System status indicator (Camera D7)

// ========== I2S Audio Pins (Shared Clock Configuration) ==========
// CRITICAL FIX: GPIO12 is a strapping pin (MTDI) and cannot be used for I2S
// Using GPIO2 for I2S RX data input to avoid hardware conflicts
// GPIO14 → INMP441 SCK and MAX98357A BCLK (shared)
// GPIO15 → INMP441 WS and MAX98357A LRC (shared)
// GPIO2  → INMP441 SD (mic data input) - SAFE PIN
// GPIO13 → MAX98357A DIN (speaker data output)
#define CONFIG_I2S_BCLK                     GPIO_NUM_14     // Bit clock (shared TX/RX)
#define CONFIG_I2S_LRCK                     GPIO_NUM_15     // Word select (shared TX/RX)
#define CONFIG_I2S_TX_DATA_OUT              GPIO_NUM_13     // MAX98357A speaker DIN
#define CONFIG_I2S_RX_DATA_IN               GPIO_NUM_2      // INMP441 mic SD (SAFE PIN!)

// ========== Camera Pins (AI-Thinker Standard) ==========
#define CONFIG_CAMERA_PIN_PWDN              GPIO_NUM_32     // Power down
#define CONFIG_CAMERA_PIN_RESET             GPIO_NUM_NC     // Reset (not used - GPIO12 is strapping pin)
#define CONFIG_CAMERA_PIN_XCLK              GPIO_NUM_0      // 20MHz clock
#define CONFIG_CAMERA_PIN_SIOD              GPIO_NUM_26     // I2C data (SCCB)
#define CONFIG_CAMERA_PIN_SIOC              GPIO_NUM_27     // I2C clock (SCCB)

// Camera parallel data bus D0-D7
#define CONFIG_CAMERA_PIN_D0                GPIO_NUM_5
#define CONFIG_CAMERA_PIN_D1                GPIO_NUM_18
#define CONFIG_CAMERA_PIN_D2                GPIO_NUM_19
#define CONFIG_CAMERA_PIN_D3                GPIO_NUM_21
#define CONFIG_CAMERA_PIN_D4                GPIO_NUM_36
#define CONFIG_CAMERA_PIN_D5                GPIO_NUM_39
#define CONFIG_CAMERA_PIN_D6                GPIO_NUM_34
#define CONFIG_CAMERA_PIN_D7                GPIO_NUM_35

// Camera sync signals
#define CONFIG_CAMERA_PIN_VSYNC             GPIO_NUM_25
#define CONFIG_CAMERA_PIN_HREF              GPIO_NUM_23
#define CONFIG_CAMERA_PIN_PCLK              GPIO_NUM_22

/*******************************************************************************
 * I2S AUDIO CONFIGURATION
 ******************************************************************************/

// Audio format (MANDATORY for Vosk STT compatibility)
#define CONFIG_AUDIO_SAMPLE_RATE            16000           // 16kHz for STT
#define CONFIG_AUDIO_BITS_PER_SAMPLE        I2S_BITS_PER_SAMPLE_16BIT
#define CONFIG_AUDIO_CHANNELS               1               // Mono

// I2S DMA buffer configuration
// FINAL FIX: Optimized to reduce DMA-capable RAM pressure while maintaining throughput
// Strategy: Fewer descriptors (less metadata overhead) + Larger buffers (maintain performance)
#define CONFIG_I2S_DMA_BUF_COUNT            4               // Number of DMA descriptors (was 16, reduced to 4 to lower memory pressure)
#define CONFIG_I2S_DMA_BUF_LEN              1200            // Samples per buffer (was 512, increased to 1200 for sustained throughput)

// I2S controller assignment
#define CONFIG_I2S_NUM_TX                   I2S_NUM_0       // Speaker output
#define CONFIG_I2S_NUM_RX                   I2S_NUM_1       // Microphone input

// Audio buffer sizes (PSRAM-backed)
#define CONFIG_STT_RING_BUFFER_SIZE         (64 * 1024)     // 64KB for 2 seconds @ 16kHz
#define CONFIG_TTS_BUFFER_SIZE              (512 * 1024)    // 512KB for TTS WAV data

/*******************************************************************************
 * CAMERA CONFIGURATION
 ******************************************************************************/

#define CONFIG_CAMERA_FRAME_SIZE            FRAMESIZE_VGA   // 640x480
#define CONFIG_CAMERA_JPEG_QUALITY          12              // 0-63 (lower = better)
#define CONFIG_CAMERA_FB_COUNT              2               // Frame buffers
#define CONFIG_CAMERA_XCLK_FREQ             20000000        // 20MHz XCLK for OV2640

// Camera initialization timeout
#define CONFIG_CAMERA_INIT_TIMEOUT_MS       5000

/*******************************************************************************
 * FREERTOS TASK PRIORITIES (Priority Hierarchy)
 ******************************************************************************/

// CRITICAL: Higher number = higher priority
#define TASK_PRIORITY_STATE_MANAGER         10      // System orchestrator (Core 1)
#define TASK_PRIORITY_I2S_AUDIO             9       // Real-time audio I/O (Core 0)
#define TASK_PRIORITY_WEBSOCKET             8       // Network I/O (Core 0)
#define TASK_PRIORITY_STT_PROCESSING        7       // Audio preprocessing (Core 1)
#define TASK_PRIORITY_CAMERA_CAPTURE        6       // Frame acquisition (Core 1)
#define TASK_PRIORITY_BUTTON_FSM            5       // Button handling (Core 0)
#define TASK_PRIORITY_TTS_DECODER           5       // TTS WAV parsing (Core 1)

// Core affinity
#define TASK_CORE_PRO                       0       // Core 0 - I/O operations
#define TASK_CORE_APP                       1       // Core 1 - Processing

/*******************************************************************************
 * BUTTON FSM CONFIGURATION
 ******************************************************************************/

#define CONFIG_BUTTON_DEBOUNCE_MS           50              // Debounce time
#define CONFIG_BUTTON_LONG_PRESS_MS         3000            // Long press threshold
#define CONFIG_BUTTON_DOUBLE_CLICK_MAX_MS   250             // Double-click window

/*******************************************************************************
 * NETWORK CONFIGURATION (Using Kconfig - run 'idf.py menuconfig' to change)
 ******************************************************************************/

// Build WebSocket URI from Kconfig variables
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// These are now configured via menuconfig (see main/Kconfig.projbuild)
// To change: Run 'idf.py menuconfig' -> "HotPin Network Configuration"
#define CONFIG_WEBSOCKET_URI                "ws://" CONFIG_HOTPIN_SERVER_IP ":" TOSTRING(CONFIG_HOTPIN_SERVER_PORT) "/ws"
#define CONFIG_WEBSOCKET_SESSION_ID         CONFIG_HOTPIN_SESSION_ID
#define CONFIG_WEBSOCKET_RECONNECT_DELAY_MS 5000
#define CONFIG_WEBSOCKET_TIMEOUT_MS         30000
#define CONFIG_AUTH_BEARER_TOKEN            CONFIG_HOTPIN_AUTH_TOKEN

/*******************************************************************************
 * WIFI CONFIGURATION (Using Kconfig)
 ******************************************************************************/

#define CONFIG_WIFI_SSID                    CONFIG_HOTPIN_WIFI_SSID
#define CONFIG_WIFI_PASSWORD                CONFIG_HOTPIN_WIFI_PASSWORD
#define CONFIG_WIFI_MAXIMUM_RETRY           5
#define CONFIG_WIFI_CONN_TIMEOUT_MS         10000

/*******************************************************************************
 * HTTP SERVER CONFIGURATION (Using Kconfig)
 ******************************************************************************/

// Automatically uses same IP as WebSocket
#define CONFIG_HTTP_SERVER_URL              "http://" CONFIG_HOTPIN_SERVER_IP ":" TOSTRING(CONFIG_HOTPIN_SERVER_PORT)
#define CONFIG_HTTP_IMAGE_ENDPOINT          "/image"                      // Image upload endpoint
#define CONFIG_HTTP_TIMEOUT_MS              30000                         // HTTP request timeout

/*******************************************************************************
 * MEMORY ALLOCATION HELPERS
 ******************************************************************************/

// PSRAM allocation with DMA capability
#define MALLOC_CAP_PSRAM_DMA        (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)

/*******************************************************************************
 * DEBUG CONFIGURATION
 ******************************************************************************/

#define CONFIG_ENABLE_DEBUG_LOGS            1
#define CONFIG_LOG_LEVEL                    ESP_LOG_INFO

// Component-specific log tags
#define TAG_MAIN                            "HOTPIN_MAIN"
#define TAG_STATE_MGR                       "STATE_MGR"
#define TAG_CAMERA                          "CAMERA"
#define TAG_AUDIO                           "AUDIO"
#define TAG_WEBSOCKET                       "WEBSOCKET"
#define TAG_BUTTON                          "BUTTON"
#define TAG_STT                             "STT"
#define TAG_TTS                             "TTS"

/*******************************************************************************
 * VALIDATION MACROS
 ******************************************************************************/

// Compile-time assertions
_Static_assert(CONFIG_AUDIO_SAMPLE_RATE == 16000, 
               "Sample rate must be 16kHz for Vosk compatibility");

_Static_assert(CONFIG_I2S_DMA_BUF_COUNT >= 4, 
               "Minimum 4 DMA buffers required for stable audio");

#endif // CONFIG_H
