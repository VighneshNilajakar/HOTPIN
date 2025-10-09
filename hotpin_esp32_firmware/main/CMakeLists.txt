# Main component CMakeLists.txt
# Registers all source files and links required ESP-IDF components

# Register all source files
idf_component_register(
    SRCS 
        "main.c"
        "button_handler.c"
        "camera_controller.c"
        "audio_driver.c"
        "websocket_client.c"
        "state_manager.c"
        "stt_pipeline.c"
        "tts_decoder.c"
        "http_client.c"
        "json_protocol.c"
        "led_controller.c"
        "serial_commands.c"
    
    INCLUDE_DIRS 
        "include"
    
    REQUIRES
        driver              # GPIO, I2S, SPI
        esp_wifi            # WiFi connectivity
        nvs_flash           # Non-volatile storage
        esp_http_client     # HTTP client for REST API
        esp32-camera        # Camera driver (managed component)
        esp_websocket_client # WebSocket client (managed component)
        freertos            # FreeRTOS kernel
        esp_system          # System APIs
        esp_common          # Common ESP-IDF utilities
        esp_timer           # Timer APIs
        heap                # Heap memory management
        soc                 # SoC-specific APIs
        json                # JSON parsing (cJSON)
        tcp_transport       # TCP transport for WebSocket
        esp_event           # Event system
        esp_netif           # Network interface
        spi_flash           # SPI flash APIs (for esp_flash.h)
        esp_psram           # PSRAM APIs (for esp_psram.h)
)

# Set C standard to C11
set_source_files_properties(
    main.c
    button_handler.c
    camera_controller.c
    audio_driver.c
    websocket_client.c
    state_manager.c
    stt_pipeline.c
    tts_decoder.c
    http_client.c
    json_protocol.c
    led_controller.c
    PROPERTIES COMPILE_FLAGS "-std=gnu11 -Wall -Wextra"
)
