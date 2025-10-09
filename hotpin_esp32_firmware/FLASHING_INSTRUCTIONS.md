# ESP32-CAM Firmware Upload Instructions

## Complete Step-by-Step Guide for Windows + ESP-IDF

---

## Prerequisites

### 1. ESP-IDF Installation
- **Download**: https://dl.espressif.com/dl/esp-idf/
- **Version Required**: ESP-IDF v5.4.2 (installed at `C:\Espressif\frameworks\esp-idf-v5.4.2`)
- **Components**: Toolchain, CMake, Ninja, Python are automatically installed

### 2. USB-UART Driver
Install appropriate driver for your adapter:
- **CH340**: https://www.wch.cn/downloads/CH341SER_ZIP.html
- **CP210x**: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers

### 3. Git Configuration Fix (One-Time Setup)
If you see "dubious ownership" errors, run:
```powershell
git config --global --add safe.directory C:/Espressif/frameworks/esp-idf-v5.4.2
```

---

## Hardware Setup

### ESP32-CAM Wiring for Flashing

**CRITICAL**: ESP32-CAM boards typically don't have USB. Use USB-UART adapter:

| ESP32-CAM Pin | USB-UART Adapter | Notes |
|---------------|------------------|-------|
| **5V** | 5V | **Stable supply ≥ 500mA required** |
| **GND** | GND | Common ground |
| **U0R (RX)** | TXD | ESP32 receives from UART TX |
| **U0T (TX)** | RXD | ESP32 transmits to UART RX |
| **IO0** | GND | **Connect to GND ONLY while entering flash mode** |

### Flash Mode Sequence
1. **Hold** IO0 connected to GND (or press IO0 button if available)
2. **Press and release** RST button (or power cycle the board)
3. **Release** IO0 after you see "Connecting..." in terminal
4. IO0 can stay grounded during flash; disconnect after completion

**Tip**: Some ESP32-CAM dev boards have built-in buttons for IO0 and RST.

---

## Build & Flash Process

### Step 1: Open ESP-IDF PowerShell

**CRITICAL**: Must use ESP-IDF terminal, NOT regular PowerShell!

- Windows Start Menu → Search "ESP-IDF" → Click **"ESP-IDF PowerShell"**

This terminal auto-configures PATH and IDF_PATH environment variables.

---

### Step 2: Navigate to Project Directory

```powershell
cd "F:\Documents\College\6th Semester\Project\ESP_Warp\hotpin_esp32_firmware"
```

---

### Step 3: Set Target Platform

Run once to configure for ESP32:

```powershell
idf.py set-target esp32
```

**Expected output**:
```
Set Target to: esp32, new sdkconfig will be created.
-- Building ESP-IDF components for target esp32
-- Project will be built for target esp32
```

**Note**: This triggers the ESP-IDF Component Manager to download managed components (`esp32-camera` and `esp_websocket_client`) as specified in `main/idf_component.yml`. You should see output like:
```
Processing 2 dependencies:
[1/2] espressif/esp32-camera (2.0.x)
[2/2] espressif/esp_websocket_client (1.x.x)
```

**If components don't download**: Delete the `build/` folder and ensure you have internet connectivity.

---

### Step 4: Configure Project (menuconfig)

Launch configuration menu:

```powershell
idf.py menuconfig
```

#### Critical Settings to Configure:

**A. Enable PSRAM (MANDATORY for ESP32-WROVER)**
```
Component config → ESP32-specific
  → [x] Support for external, SPI-connected RAM
  
Component config → ESP PSRAM
  → [x] Support for external, SPI-connected RAM
  → PSRAM speed: 40MHz (or 80MHz if stable)
  → [x] Allow DMA access to external RAM
```

**B. Disable SD/MMC Host (CRITICAL - Frees GPIOs 2, 4, 12, 13, 14, 15)**
```
Component config → SD/MMC
  → [ ] MMC/SDIO Host Support (DISABLE/UNCHECK)
```

**C. Camera Configuration**
```
Component config → Camera
  → (Should be auto-configured for AI-Thinker pins)
```

**D. Flash Configuration**
```
Serial flasher config
  → Flash Size: 4MB (or match your module - check with magnifying glass)
  → Flash Mode: QIO (fallback to DIO if unstable)
  → Flash Speed: 80MHz (fallback to 40MHz if errors)
  → Baud rate: 921600 (can reduce to 460800 if unreliable)
```

**E. Partition Table**
```
Partition Table
  → Partition Table: Single factory app, no OTA
  (Or choose "Factory app, two OTA" for future OTA updates)
```

**Save & Exit**: Press `S` → `Enter` → `Q`

---

### Step 5: Update WiFi & Server Configuration

Edit `main/include/config.h`:

```c
// WiFi credentials
#define CONFIG_WIFI_SSID         "YourActualWiFiSSID"
#define CONFIG_WIFI_PASSWORD     "YourActualPassword"

// WebSocket server (update IP address)
#define CONFIG_WEBSOCKET_URI     "ws://192.168.1.100:8000/ws"
#define CONFIG_HTTP_SERVER_URL   "http://192.168.1.100:8000"

// Optional: Update session ID
#define CONFIG_WEBSOCKET_SESSION_ID  "esp32-cam-hotpin-001"

// Optional: Add authentication token
#define CONFIG_AUTH_BEARER_TOKEN     "your_api_token_here"
```

**Security Note**: Never commit real credentials to version control!

---

### Step 6: Build the Firmware

```powershell
idf.py build
```

**Build process will**:
1. Download managed components (esp32-camera, esp_websocket_client)
2. Compile all source files
3. Link libraries
4. Generate binary files

**Expected successful output**:
```
Project build complete. To flash, run this command:
idf.py -p (PORT) flash
```

**Build artifacts location**: `build/hotpin_esp32_firmware.bin`

**If build fails**:
- Check error messages carefully
- Verify all components downloaded successfully
- Run `idf.py fullclean` then `idf.py build` again

---

### Step 7: Identify COM Port

Find which COM port your USB-UART adapter is using:

```powershell
Get-PnpDevice -Class Ports | Select-Object FriendlyName
```

**Example output**:
```
USB-SERIAL CH340 (COM7)
```

Note your COM port number (e.g., COM7, COM3, etc.)

---

### Step 8: Enter Flash Mode (Hardware)

**Before flashing**:
1. Connect IO0 to GND (or hold IO0 button)
2. Press and release RST button (or power cycle)
3. Board is now in bootloader mode
4. You can release IO0 after "Connecting..." appears

**Verification**: If successful, you'll see rapid blinking on the built-in LED.

---

### Step 9: Flash & Monitor

Flash the firmware and open serial monitor:

```powershell
idf.py -p COM7 flash monitor
```

Replace `COM7` with your actual port.

**Alternative with custom baud rate**:
```powershell
idf.py -p COM7 -b 921600 flash monitor
```

**What happens**:
1. Bootloader flashed at 0x1000
2. Partition table flashed at 0x8000
3. Application flashed at 0x10000
4. Serial monitor automatically opens

**Exit Monitor**: Press `Ctrl + ]`

---

### Step 10: Monitor Only (After Flash)

To monitor logs without reflashing:

```powershell
idf.py -p COM7 monitor
```

**Useful monitor filters**:
```powershell
# Show only errors and warnings
idf.py -p COM7 monitor --monitor-filters=error,warning

# Colored output with timestamps
idf.py -p COM7 monitor --monitor-filter=log,color
```

---

## Expected Boot Sequence

### Successful Boot Logs:

```
ESP-ROM:esp32s0
Build:Sep 19 2019
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)

I (xxx) cpu_start: Pro cpu up.
I (xxx) cpu_start: Starting app cpu, entry point is 0x...
I (xxx) cpu_start: App cpu up.
I (xxx) cpu_start: Pro cpu start user code
I (xxx) spiram: Found 4MB SPI RAM, initialized
I (xxx) spiram: SPI SRAM memory test OK

I (xxx) HOTPIN_MAIN: ================================================
I (xxx) HOTPIN_MAIN:     HotPin ESP32-CAM AI Agent - INITIALIZING
I (xxx) HOTPIN_MAIN: ================================================

I (xxx) HOTPIN_MAIN: Disabling brownout detector
I (xxx) HOTPIN_MAIN: Configuring GPIO 4 (Flash LED) to LOW
I (xxx) HOTPIN_MAIN: Holding GPIO 4 state via RTC hold

I (xxx) WIFI: Initializing WiFi...
I (xxx) WIFI: WiFi started, connecting to SSID: YourSSID
I (xxx) WIFI: WiFi connected successfully
I (xxx) WIFI: IP Address: 192.168.1.XX

I (xxx) WEBSOCKET: Initializing WebSocket client...
I (xxx) WEBSOCKET: Server URI: ws://192.168.1.100:8000/ws
I (xxx) WEBSOCKET: ✅ WebSocket client initialized

I (xxx) CAMERA: Initializing camera controller...
I (xxx) CAMERA: ✅ Camera initialized successfully (VGA 640x480)

I (xxx) AUDIO: Audio driver manager ready for mode switching
I (xxx) BUTTON: Button handler initialized on GPIO 4

I (xxx) STATE_MGR: State manager started - Default: CAMERA_STANDBY

I (xxx) HOTPIN_MAIN: ================================================
I (xxx) HOTPIN_MAIN:     ✅ SYSTEM INITIALIZATION COMPLETE
I (xxx) HOTPIN_MAIN: ================================================
```

---

## Verification & Testing

### 1. PSRAM Check
Look for:
```
I (xxx) spiram: Found 4MB SPI RAM
```
**If not found**: Your module doesn't have PSRAM (ESP32-WROVER required).

### 2. WiFi Connection
Look for:
```
I (xxx) WIFI: WiFi connected successfully
I (xxx) WIFI: IP Address: 192.168.1.XX
```
**If failed**: Check SSID/password in config.h

### 3. Camera Initialization
Look for:
```
I (xxx) CAMERA: ✅ Camera initialized successfully
```
**If failed**: Check GPIO pin conflicts or PSRAM availability

### 4. WebSocket Connection
Look for:
```
I (xxx) WEBSOCKET: ✅ WebSocket connected to server
I (xxx) WEBSOCKET: Handshake sent: {"session_id":"esp32-cam-hotpin-001"}
```
**If failed**: Verify server is running and IP address is correct

### 5. Button Test
- **Single click**: Should toggle between CAMERA and VOICE modes
- **Long press (3s)**: Should trigger shutdown sequence

---

## Troubleshooting

### Issue 1: "Failed to connect to ESP32"
**Symptoms**: `A fatal error occurred: Failed to connect to ESP32`

**Solutions**:
1. Verify IO0 is grounded during boot
2. Check USB-UART wiring (RX/TX might be swapped)
3. Ensure GND is common between ESP32 and adapter
4. Try lower baud rate: `idf.py -p COM7 -b 115200 flash`
5. Power cycle and retry

### Issue 2: "Brownout detector was triggered"
**Symptoms**: Constant resets, unstable operation

**Solutions**:
1. Use external 5V power supply (not USB power)
2. Ensure supply can provide ≥ 500mA
3. Add 100μF capacitor across 5V and GND
4. Disable brownout in code (already done in `main.c`)

### Issue 3: "Camera init failed - no PSRAM"
**Symptoms**: `E (xxx) CAMERA: esp_camera_init failed: ESP_ERR_NO_MEM`

**Solutions**:
1. Verify module is ESP32-WROVER (has PSRAM chip)
2. Check menuconfig: PSRAM support enabled
3. Look for boot log: "Found 4MB SPI RAM"
4. Standard ESP32-CAM modules often lack PSRAM

### Issue 4: "WebSocket connection refused"
**Symptoms**: `E (xxx) WEBSOCKET: Failed to connect`

**Solutions**:
1. Verify server is running: `python main.py` in server folder
2. Check firewall isn't blocking port 8000
3. Verify IP address matches server IP
4. Ping server from ESP32's network
5. Check server logs for connection attempts

### Issue 5: "GPIO 4 Flash LED always on"
**Symptoms**: Bright flash LED during boot

**Solutions**:
- Already mitigated with `rtc_gpio_hold_en(GPIO_NUM_4)` in firmware
- If persists, add physical pull-down resistor (10kΩ) on GPIO4

### Issue 6: "Guru Meditation Error: LoadProhibited"
**Symptoms**: Crash during mode switching

**Solutions**:
1. Ensure proper driver deinitialization in state machine
2. Check I2S mutex is properly acquired before driver operations
3. Verify no overlapping GPIO usage
4. Monitor stack usage in FreeRTOS tasks

### Issue 7: Build fails with "component not found"
**Symptoms**: CMake errors about missing `esp32-camera` or `esp_websocket_client`

**Solutions**:
1. Verify `main/idf_component.yml` exists (not just root `idf_component.yml`)
2. Check internet connection (component manager needs to download)
3. Delete `build/` folder manually if it exists, then run: `idf.py set-target esp32`
4. Update component manager: `python -m pip install --upgrade idf-component-manager`
5. If still failing, manually add dependencies:
   ```powershell
   idf.py add-dependency "espressif/esp32-camera^2.0.0"
   idf.py add-dependency "espressif/esp_websocket_client^1.0.0"
   ```

---

## Advanced Options

### 1. Faster Flashing
After initial flash, skip bootloader/partition table:
```powershell
idf.py -p COM7 app-flash
```

### 2. Manual Flash with esptool
If `idf.py flash` fails:
```powershell
python $env:IDF_PATH\components\esptool_py\esptool\esptool.py `
  --chip esp32 --port COM7 --baud 460800 `
  write_flash -z `
  0x1000 build\bootloader\bootloader.bin `
  0x8000 build\partition_table\partition-table.bin `
  0x10000 build\hotpin_esp32_firmware.bin
```

### 3. Erase Flash Completely
To start fresh:
```powershell
idf.py -p COM7 erase-flash
```

### 4. Read Flash
To backup current firmware:
```powershell
esptool.py --port COM7 read_flash 0 0x400000 backup.bin
```

### 5. Enable Core Dumps
For debugging crashes, add to menuconfig:
```
Component config → Core dump
  → [x] Core dump to flash
```

---

## OTA Updates (Future Enhancement)

To enable Over-The-Air updates:

1. **Change partition table** in menuconfig:
   ```
   Partition Table → Factory app, two OTA definitions
   ```

2. **Implement OTA handler** using `esp_https_ota` component

3. **Host update binary** on server at `/firmware/update.bin`

4. **Trigger update** via button long-press or server command

---

## Performance Optimization

### For Faster Boot:
```
menuconfig → Bootloader config → Bootloader log verbosity: No output
menuconfig → Compiler options → Optimization Level: Release (-O2)
```

### For Debugging:
```
menuconfig → Compiler options → Optimization Level: Debug (-Og)
menuconfig → Component config → Log output → Default log verbosity: Debug
```

---

## Quick Command Reference

```powershell
# Setup (one-time)
cd "F:\Documents\College\6th Semester\Project\ESP_Warp\hotpin_esp32_firmware"
idf.py set-target esp32
idf.py menuconfig

# Build & Flash cycle
idf.py build
idf.py -p COM7 flash monitor

# Monitor only
idf.py -p COM7 monitor

# Clean rebuild
idf.py fullclean
idf.py build

# App-only flash (faster)
idf.py -p COM7 app-flash

# Erase everything
idf.py -p COM7 erase-flash

# Exit monitor
Ctrl + ]
```

---

## Security Best Practices

1. **Never commit credentials**: Use `.gitignore` for `config.h` or create `secrets.h`
2. **Use WSS instead of WS**: Implement TLS for WebSocket in production
3. **Rotate API tokens**: Change `CONFIG_AUTH_BEARER_TOKEN` regularly
4. **Firmware encryption**: Enable flash encryption in menuconfig for production
5. **Secure Boot**: Enable in menuconfig to prevent unauthorized firmware

---

## Hardware Checklist Before Flash

- [ ] Module is ESP32-WROVER (has PSRAM chip visible)
- [ ] USB-UART adapter provides stable 5V ≥ 500mA
- [ ] All wiring connections secure (5V, GND, TX, RX)
- [ ] IO0 mechanism ready (button or jumper wire)
- [ ] SD card removed (to avoid GPIO conflicts)
- [ ] Correct COM port identified
- [ ] ESP-IDF PowerShell terminal opened

---

## Software Checklist Before Flash

- [ ] ESP-IDF v5.4.2 installed
- [ ] Git safe directory configured
- [ ] WiFi credentials updated in `config.h`
- [ ] Server IP address updated in `config.h`
- [ ] PSRAM enabled in menuconfig
- [ ] SD/MMC disabled in menuconfig
- [ ] Build completes successfully
- [ ] `main/idf_component.yml` exists with camera + websocket dependencies
- [ ] Component manager successfully downloaded dependencies during `set-target`

---

## Post-Flash Checklist

- [ ] Boot log shows PSRAM detected (4MB)
- [ ] WiFi connects and gets IP address
- [ ] Camera initializes successfully
- [ ] WebSocket connects to server
- [ ] Button responds to presses
- [ ] Status LED (GPIO33) indicates system state
- [ ] Flash LED (GPIO4) stays OFF

---

## Support Resources

- **ESP-IDF Documentation**: https://docs.espressif.com/projects/esp-idf/en/v5.4.2/
- **ESP32-CAM Pinout**: https://github.com/raphaelbs/esp32-cam-ai-thinker
- **Component Registry**: https://components.espressif.com/
- **Forum**: https://www.esp32.com/

---

## Project Structure Reference

```
hotpin_esp32_firmware/
├── CMakeLists.txt              # Root build config
├── idf_component.yml           # Managed dependencies
├── sdkconfig.defaults          # Default configuration
├── main/
│   ├── CMakeLists.txt          # Main component build
│   ├── main.c                  # Entry point
│   ├── state_manager.c         # FSM controller
│   ├── button_handler.c        # GPIO interrupt handler
│   ├── camera_controller.c     # OV2640 driver wrapper
│   ├── audio_driver.c          # I2S TX/RX manager
│   ├── websocket_client.c      # Server communication
│   ├── stt_pipeline.c          # Speech-to-Text audio preprocessing
│   ├── tts_decoder.c           # Text-to-Speech WAV parser
│   ├── http_client.c           # REST API client
│   ├── json_protocol.c         # JSON message parser
│   ├── led_controller.c        # Status LED manager
│   └── include/                # Header files
└── build/                      # Build artifacts (auto-generated)
```

---

**Last Updated**: October 7, 2025  
**Firmware Version**: 1.0.0  
**ESP-IDF Version**: v5.4.2  
**Target**: ESP32 (ESP32-WROVER-E recommended)

---

**Good luck with your HotPin wearable AI assistant project! 🚀**
