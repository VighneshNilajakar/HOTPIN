# Automatic Server IP Detection

## 🎯 Purpose

This script automatically detects your PC's IP address and WiFi network, then updates the ESP32 firmware `.env` file with the correct configuration. No more manual IP hunting!

## ✨ Features

- ✅ **Auto-detect Server IP** - Finds your PC's active network IP address
- ✅ **Auto-detect WiFi SSID** - Identifies which WiFi network you're connected to
- ✅ **Auto-update .env** - Updates the configuration file automatically
- ✅ **Cross-platform** - Works on Windows (WiFi detection Windows-only)
- ✅ **Safe** - Only updates specific fields, preserves WiFi password and other settings

## 📋 Usage

### Quick Start

From the firmware directory:
```bash
python scripts/update_server_ip.py
```

### From Project Root

```bash
cd hotpin_esp32_firmware
python scripts/update_server_ip.py
```

### Complete Workflow

```bash
# Step 1: Auto-detect and update IP/WiFi
python scripts/update_server_ip.py

# Step 2: Apply configuration to firmware
python scripts/apply_env_config.py

# Step 3: Build and flash
idf.py build flash monitor
```

## 📊 Example Output

```
============================================================
🔍 Automatic Server IP Detection & Configuration
============================================================

📡 Detecting local IP address...
✓ Detected IP: 172.24.158.58

📶 Detecting WiFi network...
✓ Connected to: Darshan

📝 Updating configuration file...
   File: F:\...\hotpin_esp32_firmware\.env

  ✓ Updated HOTPIN_SERVER_IP=172.24.158.58
  ✓ Updated HOTPIN_WIFI_SSID=Darshan
✅ Successfully updated .env file

============================================================
✅ Configuration Updated Successfully!
============================================================

📋 Summary:
   Server IP: 172.24.158.58
   WiFi Network: Darshan

🔧 Next steps:
   1. Verify WiFi password in .env file
   2. Run: python scripts/apply_env_config.py
   3. Run: idf.py build flash monitor
```

## 🔧 How It Works

### IP Detection

1. **Primary Method**: Creates a socket connection test to Google DNS (8.8.8.8)
   - Determines which network interface would be used
   - Returns that interface's IP address
   - Doesn't actually send data, just queries routing table

2. **Fallback Method**: Uses `socket.gethostbyname(socket.gethostname())`
   - Queries system hostname
   - Returns associated IP address

### WiFi Detection (Windows)

Uses Windows `netsh` command:
```bash
netsh wlan show interfaces
```

Parses output to extract SSID of currently connected network.

### .env File Update

1. Reads existing `.env` file line by line
2. Finds lines starting with `HOTPIN_SERVER_IP=` and `HOTPIN_WIFI_SSID=`
3. Replaces values while preserving formatting
4. Writes updated content back to file
5. **Preserves** all other settings (password, session ID, etc.)

## 🎨 What Gets Updated

| Field | Updated | Preserved |
|-------|---------|-----------|
| `HOTPIN_SERVER_IP` | ✅ Yes | - |
| `HOTPIN_WIFI_SSID` | ✅ Yes | - |
| `HOTPIN_SERVER_PORT` | - | ✅ Yes |
| `HOTPIN_WIFI_PASSWORD` | - | ✅ Yes |
| `HOTPIN_SESSION_ID` | - | ✅ Yes |
| `HOTPIN_AUTH_TOKEN` | - | ✅ Yes |

**Important:** Always verify your WiFi password is correct after running the script!

## 🚨 Troubleshooting

### "Failed to detect local IP address"

**Cause:** No active network connection

**Solutions:**
1. Check WiFi/Ethernet is connected
2. Ping a website to test connectivity: `ping google.com`
3. Restart network adapter
4. Run script again

### "Could not detect WiFi SSID"

**Cause:** Using Ethernet or non-Windows OS

**Solution:**
- Script will still update IP address
- Manually edit `.env` to set `HOTPIN_WIFI_SSID`

### "HOTPIN_SERVER_IP not found in .env file"

**Cause:** `.env` file format is incorrect or corrupted

**Solutions:**
1. Check `.env` file exists in `hotpin_esp32_firmware/` directory
2. Verify file has `HOTPIN_SERVER_IP=` line (even if empty)
3. Restore from `.env.example` if needed

## 📁 File Locations

```
hotpin_esp32_firmware/
├── .env                           ← Updated by this script
├── .env.example                   ← Template (not modified)
└── scripts/
    ├── update_server_ip.py        ← This script
    └── apply_env_config.py        ← Run after this script
```

## 🔄 Integration with Workflow

### Manual Configuration (Old Way)

```bash
# 1. Find IP manually
ipconfig  # or ifconfig on Linux

# 2. Edit .env manually
notepad .env

# 3. Apply config
python scripts/apply_env_config.py

# 4. Build
idf.py build flash
```

### Automatic Configuration (New Way)

```bash
# 1. Auto-detect and update (combines steps 1-2!)
python scripts/update_server_ip.py

# 2. Apply config
python scripts/apply_env_config.py

# 3. Build
idf.py build flash
```

**Saves 2 manual steps!** 🎉

## 🌐 Platform Support

| Platform | IP Detection | WiFi Detection | Status |
|----------|--------------|----------------|--------|
| Windows | ✅ Supported | ✅ Supported | Fully Working |
| Linux | ✅ Supported | ⚠️ Partial* | IP works, SSID manual |
| macOS | ✅ Supported | ⚠️ Partial* | IP works, SSID manual |

\* WiFi SSID detection could be added for other platforms using:
- Linux: `iwgetid -r` or `nmcli`
- macOS: `/System/Library/PrivateFrameworks/Apple80211.framework/Resources/airport -I`

## 🔒 Security Notes

- Script only **reads** network configuration (no modifications to system)
- No network requests sent (only socket routing test)
- `.env` file should remain in `.gitignore` (contains WiFi password)
- Script does **not** expose credentials outside your machine

## 🚀 Future Enhancements

Potential improvements:
- [ ] Add Linux WiFi detection (`iwgetid`)
- [ ] Add macOS WiFi detection (`airport`)
- [ ] Validate WiFi password (test connection)
- [ ] Detect multiple network interfaces and let user choose
- [ ] GUI version for non-technical users
- [ ] Integration with VSCode task runner

## 📚 Related Scripts

- **`apply_env_config.py`** - Applies `.env` values to `sdkconfig`
- **`update_server_ip.py`** - This script (auto-detect IP)

Use both together:
```bash
python scripts/update_server_ip.py && python scripts/apply_env_config.py
```

## 💡 Pro Tips

### Quick Alias (PowerShell)

Add to your PowerShell profile:
```powershell
function Update-ESP32Config {
    cd "F:\...\hotpin_esp32_firmware"
    python scripts/update_server_ip.py
    python scripts/apply_env_config.py
}
```

Then just run: `Update-ESP32Config`

### Quick Alias (Bash/Linux)

Add to `.bashrc` or `.zshrc`:
```bash
alias esp32config='cd ~/hotpin_esp32_firmware && python scripts/update_server_ip.py && python scripts/apply_env_config.py'
```

Then just run: `esp32config`

### VS Code Task

Add to `.vscode/tasks.json`:
```json
{
    "label": "Update ESP32 Config",
    "type": "shell",
    "command": "python",
    "args": [
        "scripts/update_server_ip.py"
    ],
    "options": {
        "cwd": "${workspaceFolder}/hotpin_esp32_firmware"
    }
}
```

Run with: `Ctrl+Shift+P` → `Tasks: Run Task` → `Update ESP32 Config`

## 📞 Support

If you encounter issues:
1. Check network connection
2. Verify `.env` file exists and has correct format
3. Run with elevated permissions if needed
4. Check Python version (3.6+ required)

---

**Made with ❤️ to simplify ESP32 configuration**
