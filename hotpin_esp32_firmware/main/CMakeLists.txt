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
    
    INCLUDE_DIRS 
        "include"
    
    REQUIRES
        driver              # GPIO, I2S, SPI
        esp_wifi            # WiFi connectivity
        nvs_flash           # Non-volatile storage
        esp_http_client     # HTTP client for WebSocket
        esp_websocket_client # WebSocket client library
        esp_camera          # Camera driver
        freertos            # FreeRTOS kernel
        esp_system          # System APIs
        esp_common          # Common ESP-IDF utilities
        esp_timer           # Timer APIs
        esp_ringbuf         # Ring buffer for audio
        heap                # Heap memory management
        soc                 # SoC-specific APIs
        json                # JSON parsing (cJSON)
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
    PROPERTIES COMPILE_FLAGS "-std=c11 -Wall -Wextra"
)
