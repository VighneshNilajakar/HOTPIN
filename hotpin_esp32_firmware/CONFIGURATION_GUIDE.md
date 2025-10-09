# 🔧 HotPin Network Configuration Guide

## 📋 Overview

The HotPin firmware now supports **flexible environment-based configuration**! You can easily change server IPs, WiFi credentials, and other settings without editing C header files.

## 🎯 Configuration Methods

### **Method 1: Using .env File (Easiest!) ⭐**

Perfect for quick changes and testing different servers.

#### Steps:
```bash
# 1. Edit the .env file
nano .env  # or use any text editor

# 2. Change the values you need:
#    HOTPIN_SERVER_IP=10.240.253.58
#    HOTPIN_SERVER_PORT=8000
#    HOTPIN_WIFI_SSID=YourNetworkName
#    etc.

# 3. Apply the configuration
python scripts/apply_env_config.py

# 4. Rebuild and flash
idf.py build
idf.py -p COM_PORT flash
```

#### What gets updated:
- ✅ WebSocket URI: `ws://[IP]:[PORT]/ws`
- ✅ HTTP Server URL: `http://[IP]:[PORT]`
- ✅ WiFi SSID and Password
- ✅ Session ID and Auth Token

---

### **Method 2: Using menuconfig (ESP-IDF Standard)**

Best for permanent configuration changes.

#### Steps:
```bash
# 1. Open configuration menu
idf.py menuconfig

# 2. Navigate to:
#    "HotPin Network Configuration"

# 3. Edit the values using arrow keys and Enter

# 4. Save and exit (press 'S' then 'Q')

# 5. Build and flash
idf.py build
idf.py -p COM_PORT flash
```

#### Available Options:
```
HotPin Network Configuration
├── Server IP Address         (default: 10.240.253.58)
├── Server Port               (default: 8000)
├── WiFi SSID                 (default: SGF14)
├── WiFi Password             (default: 12345678vn)
├── WebSocket Session ID      (default: esp32-cam-hotpin-001)
└── API Authentication Token  (default: your_api_token_here)
```

---

### **Method 3: Direct sdkconfig Editing**

For advanced users who want manual control.

#### Steps:
```bash
# 1. Open sdkconfig file
nano sdkconfig

# 2. Find and edit these lines:
CONFIG_HOTPIN_SERVER_IP="10.240.253.58"
CONFIG_HOTPIN_SERVER_PORT=8000
CONFIG_HOTPIN_WIFI_SSID="SGF14"
CONFIG_HOTPIN_WIFI_PASSWORD="12345678vn"
CONFIG_HOTPIN_SESSION_ID="esp32-cam-hotpin-001"
CONFIG_HOTPIN_AUTH_TOKEN="your_api_token_here"

# 3. Save and rebuild
idf.py build
idf.py -p COM_PORT flash
```

---

## 📝 Configuration Variables

### Server Configuration
| Variable | Description | Example |
|----------|-------------|---------|
| `HOTPIN_SERVER_IP` | IP address of Python server | `10.240.253.58` |
| `HOTPIN_SERVER_PORT` | Port number | `8000` |

**Auto-generated values:**
- WebSocket URI: `ws://10.240.253.58:8000/ws`
- HTTP Server URL: `http://10.240.253.58:8000`

### WiFi Configuration
| Variable | Description | Example |
|----------|-------------|---------|
| `HOTPIN_WIFI_SSID` | Network name | `SGF14` |
| `HOTPIN_WIFI_PASSWORD` | Network password | `12345678vn` |

### Device Configuration
| Variable | Description | Example |
|----------|-------------|---------|
| `HOTPIN_SESSION_ID` | Unique device identifier | `esp32-cam-hotpin-001` |
| `HOTPIN_AUTH_TOKEN` | API authentication token | `your_api_token_here` |

---

## 🚀 Quick Start Example

### Scenario: Server IP changed from 10.240.253.58 to 192.168.1.150

**Using .env method:**
```bash
# 1. Edit .env file
echo "HOTPIN_SERVER_IP=192.168.1.150" >> .env

# 2. Apply changes
python scripts/apply_env_config.py

# 3. You'll see:
#    📝 Updating: sdkconfig
#    ✓ Updated CONFIG_HOTPIN_SERVER_IP
#    ✅ Successfully updated 1 configuration(s)

# 4. Rebuild
idf.py build flash
```

That's it! The firmware will now connect to:
- WebSocket: `ws://192.168.1.150:8000/ws`
- HTTP: `http://192.168.1.150:8000`

---

## 🔄 Workflow Comparison

### Old Way (Hard-coded in config.h):
```
❌ Edit main/include/config.h
❌ Find #define CONFIG_WEBSOCKET_URI "ws://..."
❌ Change IP address manually
❌ Find #define CONFIG_HTTP_SERVER_URL "http://..."
❌ Change IP address again (must match!)
❌ Rebuild
⏱️  Time: ~5 minutes, error-prone
```

### New Way (.env file):
```
✅ Edit .env file (one line: HOTPIN_SERVER_IP=...)
✅ Run: python scripts/apply_env_config.py
✅ Rebuild
⏱️  Time: ~1 minute, automatic consistency
```

---

## 🛠️ Advanced Usage

### Multiple Device Profiles

Create different .env files for different deployments:

```bash
# Production environment
cp .env .env.production
# Edit .env.production with production server IP

# Development environment
cp .env .env.development
# Edit .env.development with local server IP

# Switch between environments:
cp .env.production .env
python scripts/apply_env_config.py
idf.py build flash
```

### Automated Deployment Script

Create a `deploy.sh` script:
```bash
#!/bin/bash
# deploy.sh - Automated deployment script

ENV_FILE=$1
PORT=$2

if [ -z "$ENV_FILE" ] || [ -z "$PORT" ]; then
    echo "Usage: ./deploy.sh <env-file> <port>"
    echo "Example: ./deploy.sh .env.production COM5"
    exit 1
fi

echo "🚀 Deploying with $ENV_FILE to $PORT"

# Apply configuration
cp $ENV_FILE .env
python scripts/apply_env_config.py

# Build and flash
idf.py build
idf.py -p $PORT flash monitor
```

Usage:
```bash
chmod +x deploy.sh
./deploy.sh .env.production COM5
```

---

## 🐛 Troubleshooting

### Problem: "CONFIG_HOTPIN_* not found in sdkconfig"

**Solution:** Run menuconfig first to generate the options:
```bash
idf.py menuconfig
# Navigate to "HotPin Network Configuration"
# Save and exit
# Then run: python scripts/apply_env_config.py
```

### Problem: "Changes not taking effect"

**Solution:** Make sure to rebuild after changing configuration:
```bash
python scripts/apply_env_config.py
idf.py build  # ← Don't forget this step!
idf.py flash
```

### Problem: "apply_env_config.py not found"

**Solution:** Run from project root directory:
```bash
cd hotpin_esp32_firmware
python scripts/apply_env_config.py
```

---

## 📂 File Structure

```
hotpin_esp32_firmware/
├── .env                           # Your custom configuration (gitignored)
├── .env.example                   # Example configuration template
├── sdkconfig                      # Generated ESP-IDF config
├── main/
│   ├── Kconfig.projbuild         # Menuconfig definitions
│   └── include/
│       └── config.h              # C header (uses Kconfig variables)
└── scripts/
    └── apply_env_config.py       # .env → sdkconfig converter
```

---

## ✅ Best Practices

1. **Never commit .env to Git** - Contains sensitive credentials
2. **Use .env.example as template** - Safe to commit, documents required variables
3. **Test configuration changes** - Use serial monitor to verify connection
4. **Keep WiFi password secure** - Use environment variables, not hard-coded values
5. **Document custom settings** - Add comments in .env for team members

---

## 🔒 Security Notes

The `.env` file is automatically ignored by Git (added to `.gitignore`). This prevents accidentally committing:
- WiFi passwords
- Server IP addresses
- Authentication tokens

**Always use .env for sensitive data!**

---

## 💡 Tips

- **Find your server IP:** Run `ipconfig` (Windows) or `ifconfig` (Linux/Mac)
- **Multiple devices:** Use different session IDs (e.g., `esp32-cam-001`, `esp32-cam-002`)
- **Quick verification:** Check serial monitor for:
  ```
  I (xxx) WEBSOCKET: Server URI: ws://YOUR_IP:8000/ws
  I (xxx) HTTP_CLIENT: HTTP client initialized (server: http://YOUR_IP:8000)
  ```

---

**Configuration system designed to be:**
- ✅ Easy to use
- ✅ Version control friendly
- ✅ Team collaboration ready
- ✅ Production deployment ready
