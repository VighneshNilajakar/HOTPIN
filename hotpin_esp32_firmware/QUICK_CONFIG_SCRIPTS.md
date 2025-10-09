# Quick Configuration Scripts

## 🚀 Automated Configuration Workflow

### Step 1: Auto-Detect Server IP and WiFi
```bash
cd hotpin_esp32_firmware
python scripts/update_server_ip.py
```

**What it does:**
- ✅ Detects your PC's IP address automatically
- ✅ Detects current WiFi network (Windows)
- ✅ Updates `.env` file with correct values

**Output:**
```
✓ Detected IP: 172.24.158.58
✓ Connected to: Darshan
✓ Updated .env file
```

---

### Step 2: Apply Configuration to Firmware
```bash
python scripts/apply_env_config.py
```

**What it does:**
- ✅ Reads values from `.env` file
- ✅ Updates ESP32 `sdkconfig` file
- ✅ Prepares firmware for build

**Output:**
```
✓ Updated CONFIG_HOTPIN_SERVER_IP
✓ Updated CONFIG_HOTPIN_WIFI_SSID
✓ Updated CONFIG_HOTPIN_WIFI_PASSWORD
```

---

### Step 3: Build and Flash Firmware
```bash
idf.py build flash monitor
```

---

## ⚡ One-Line Command

Combine all configuration steps:
```bash
python scripts/update_server_ip.py && python scripts/apply_env_config.py && idf.py build flash monitor
```

---

## 📋 Manual Configuration (Old Way)

If automatic detection fails:

1. **Find your IP:**
   ```bash
   # Windows
   ipconfig
   
   # Linux/Mac
   ifconfig
   ```

2. **Edit `.env` manually:**
   ```bash
   notepad .env  # Windows
   nano .env     # Linux/Mac
   ```

3. **Apply and build:**
   ```bash
   python scripts/apply_env_config.py
   idf.py build flash monitor
   ```

---

## 🔧 Scripts Overview

| Script | Purpose | When to Use |
|--------|---------|-------------|
| `update_server_ip.py` | Auto-detect IP/WiFi | Every time network changes |
| `apply_env_config.py` | Apply .env to firmware | After editing .env |
| `idf.py` | ESP-IDF build tool | Always before flashing |

---

## ✅ Verification Checklist

After running configuration scripts:

- [ ] `.env` file has correct `HOTPIN_SERVER_IP`
- [ ] `.env` file has correct `HOTPIN_WIFI_SSID`
- [ ] `.env` file has correct `HOTPIN_WIFI_PASSWORD`
- [ ] `sdkconfig` updated (run `apply_env_config.py`)
- [ ] Firmware built successfully (`idf.py build`)
- [ ] ESP32 flashed successfully (`idf.py flash`)

Expected serial monitor output:
```
I (xxx) HOTPIN_MAIN: WiFi initialization complete, connecting to [Your WiFi]...
I (xxx) HOTPIN_MAIN: ✅ Got IP address: [ESP32 IP]
I (xxx) WEBSOCKET: Server URI: ws://[Your PC IP]:8000/ws
I (xxx) WEBSOCKET: ✅ WebSocket connected to server
```

---

## 🎯 Common Scenarios

### Scenario 1: Changed WiFi Network
```bash
# Auto-detect new network and IP
python scripts/update_server_ip.py

# Apply and rebuild
python scripts/apply_env_config.py
idf.py build flash
```

### Scenario 2: Changed Server IP (Different PC)
```bash
# Auto-detect new IP
python scripts/update_server_ip.py

# Apply and rebuild
python scripts/apply_env_config.py
idf.py build flash
```

### Scenario 3: Just Changed WiFi Password
```bash
# Edit .env manually (only password changed)
notepad .env

# Apply and rebuild
python scripts/apply_env_config.py
idf.py build flash
```

---

## 📚 Documentation

- **Detailed Guide:** `scripts/README_UPDATE_SERVER_IP.md`
- **Configuration Architecture:** `CONFIGURATION_ARCHITECTURE.md`
- **Complete Setup Guide:** `CONFIGURATION_GUIDE.md`
