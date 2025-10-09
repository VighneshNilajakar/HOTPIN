# 🔄 Migration Guide: Hard-coded → Environment-based Configuration

## 📋 For Users Updating to the New System

If you previously had hard-coded values in `config.h`, follow this guide to migrate to the new environment-based system.

---

## ✅ Quick Migration (3 Steps)

```bash
# 1. Create your .env file
cp .env.example .env

# 2. Edit with YOUR values
nano .env

# 3. Apply and rebuild
python scripts/apply_env_config.py
idf.py build flash
```

**Done!** Your existing IP addresses and WiFi credentials are now managed via `.env`.

---

## 📝 Detailed Migration

### Step 1: Identify Your Current Settings

**Before migrating**, note down your current values from `main/include/config.h`:

```c
// OLD (find these in your config.h):
#define CONFIG_WEBSOCKET_URI "ws://10.240.253.58:8000/ws"
                                   ↑↑↑↑↑↑↑↑↑↑↑↑↑↑
                                   Note your IP

#define CONFIG_WIFI_SSID "SGF14"
                         ↑↑↑↑↑
                         Note your SSID

#define CONFIG_WIFI_PASSWORD "12345678vn"
                             ↑↑↑↑↑↑↑↑↑↑
                             Note your password
```

### Step 2: Create .env File

Copy the example and edit it:

```bash
cd hotpin_esp32_firmware
cp .env.example .env
```

### Step 3: Transfer Your Values

Edit `.env` with your noted values:

```bash
# .env file
HOTPIN_SERVER_IP=10.240.253.58      # ← Your WebSocket IP
HOTPIN_SERVER_PORT=8000             # ← Your port (usually 8000)
HOTPIN_WIFI_SSID=SGF14              # ← Your WiFi name
HOTPIN_WIFI_PASSWORD=12345678vn     # ← Your WiFi password
HOTPIN_SESSION_ID=esp32-cam-hotpin-001
HOTPIN_AUTH_TOKEN=your_api_token_here
```

### Step 4: Apply Configuration

```bash
python scripts/apply_env_config.py
```

You should see:
```
============================================================
🔧 HotPin ESP32-CAM Configuration Updater
============================================================
📖 Reading configuration from: .env
  ✓ HOTPIN_SERVER_IP=10.240.253.58
  ✓ HOTPIN_SERVER_PORT=8000
  ✓ HOTPIN_WIFI_SSID=SGF14
  ✓ HOTPIN_WIFI_PASSWORD=12345678vn
  ✓ HOTPIN_SESSION_ID=esp32-cam-hotpin-001
  ✓ HOTPIN_AUTH_TOKEN=your_api_token_here

📝 Updating: sdkconfig
  ✓ Updated CONFIG_HOTPIN_SERVER_IP
  ✓ Updated CONFIG_HOTPIN_SERVER_PORT
  ✓ Updated CONFIG_HOTPIN_WIFI_SSID
  ✓ Updated CONFIG_HOTPIN_WIFI_PASSWORD
  ✓ Updated CONFIG_HOTPIN_SESSION_ID
  ✓ Updated CONFIG_HOTPIN_AUTH_TOKEN

✅ Successfully updated 6 configuration(s)
🔨 Next step: Run 'idf.py build' to rebuild with new settings
============================================================
```

### Step 5: Rebuild and Flash

```bash
idf.py build
idf.py -p COM5 flash monitor  # Replace COM5 with your port
```

### Step 6: Verify in Serial Monitor

Watch for these lines:
```
I (xxx) WEBSOCKET: Server URI: ws://10.240.253.58:8000/ws
I (xxx) HTTP_CLIENT: HTTP client initialized (server: http://10.240.253.58:8000)
```

✅ If the IP matches your `.env` file, migration successful!

---

## 🔍 What Changed?

### Code Changes (You Don't Need to Edit These!)

#### `main/include/config.h`

**BEFORE (Hard-coded):**
```c
#define CONFIG_WEBSOCKET_URI "ws://10.240.253.58:8000/ws"
#define CONFIG_HTTP_SERVER_URL "http://10.240.253.58:8000"
#define CONFIG_WIFI_SSID "SGF14"
#define CONFIG_WIFI_PASSWORD "12345678vn"
```

**AFTER (Dynamic from Kconfig):**
```c
#define CONFIG_WEBSOCKET_URI "ws://" CONFIG_HOTPIN_SERVER_IP ":" TOSTRING(CONFIG_HOTPIN_SERVER_PORT) "/ws"
#define CONFIG_HTTP_SERVER_URL "http://" CONFIG_HOTPIN_SERVER_IP ":" TOSTRING(CONFIG_HOTPIN_SERVER_PORT)
#define CONFIG_WIFI_SSID CONFIG_HOTPIN_WIFI_SSID
#define CONFIG_WIFI_PASSWORD CONFIG_HOTPIN_WIFI_PASSWORD
```

The macros now expand using Kconfig variables set by your `.env` file!

---

## 🎯 Value Mapping Reference

| Old (config.h) | New (.env) | Notes |
|---------------|-----------|-------|
| `CONFIG_WEBSOCKET_URI` IP part | `HOTPIN_SERVER_IP` | Only IP, not full URI |
| `CONFIG_WEBSOCKET_URI` port part | `HOTPIN_SERVER_PORT` | Only port number |
| `CONFIG_HTTP_SERVER_URL` | Auto-generated | From IP + PORT |
| `CONFIG_WIFI_SSID` | `HOTPIN_WIFI_SSID` | Direct mapping |
| `CONFIG_WIFI_PASSWORD` | `HOTPIN_WIFI_PASSWORD` | Direct mapping |
| `CONFIG_WEBSOCKET_SESSION_ID` | `HOTPIN_SESSION_ID` | Direct mapping |
| `CONFIG_AUTH_BEARER_TOKEN` | `HOTPIN_AUTH_TOKEN` | Direct mapping |

---

## ⚠️ Common Migration Issues

### Issue 1: "Python script not found"

**Symptom:**
```
python: can't open file 'scripts/apply_env_config.py'
```

**Solution:**
Make sure you're in the correct directory:
```bash
cd hotpin_esp32_firmware  # Project root
python scripts/apply_env_config.py
```

### Issue 2: ".env file not found"

**Symptom:**
```
❌ Error: .env not found!
💡 Tip: Copy .env.example to .env and edit the values
```

**Solution:**
```bash
cp .env.example .env
nano .env  # Edit with your values
```

### Issue 3: "CONFIG_HOTPIN_* not found in sdkconfig"

**Symptom:**
Script runs but no updates are made.

**Solution:**
Run menuconfig first to generate the options:
```bash
idf.py menuconfig
# Navigate to "HotPin Network Configuration"
# Just save and exit (no need to change values)
# Then run: python scripts/apply_env_config.py
```

### Issue 4: "Old IP still showing in serial monitor"

**Symptom:**
Serial monitor shows old IP address after migration.

**Solution:**
You forgot to rebuild! Run:
```bash
python scripts/apply_env_config.py  # Apply config
idf.py build                        # ← DON'T SKIP THIS!
idf.py flash                        # Flash new firmware
```

---

## 🎓 New Workflow After Migration

### When You Need to Change Server IP

**OLD Way (Don't do this anymore!):**
```
❌ 1. Open main/include/config.h
❌ 2. Find CONFIG_WEBSOCKET_URI line
❌ 3. Change IP manually
❌ 4. Find CONFIG_HTTP_SERVER_URL line
❌ 5. Change IP again (must match!)
❌ 6. Save and rebuild
```

**NEW Way (Do this instead!):**
```
✅ 1. Edit .env file (one line: HOTPIN_SERVER_IP=...)
✅ 2. Run: python scripts/apply_env_config.py
✅ 3. Run: idf.py build flash
```

### When WiFi Network Changes

**NEW Way:**
```bash
# Edit .env
nano .env
# Change:
#   HOTPIN_WIFI_SSID=NewNetwork
#   HOTPIN_WIFI_PASSWORD=newpassword

# Apply and deploy
python scripts/apply_env_config.py
idf.py build flash
```

---

## 📦 What to Keep in Git

### ✅ Commit These (Safe):
- `.env.example` (template)
- `main/Kconfig.projbuild` (configuration schema)
- `main/include/config.h` (updated to use Kconfig)
- `scripts/apply_env_config.py` (automation script)
- All documentation files

### ❌ Don't Commit These (Private):
- `.env` (your credentials)
- `sdkconfig` (generated)
- `sdkconfig.old` (backup)
- `build/` (compiled files)

These are already in `.gitignore`!

---

## 🔄 Reverting (If Needed)

If you need to temporarily go back to the old system:

```bash
# 1. Restore old config.h from git history
git checkout HEAD~1 main/include/config.h

# 2. Rebuild with old configuration
idf.py build flash
```

But we recommend staying with the new system - it's much easier! 😊

---

## ✅ Migration Checklist

- [ ] Noted down current IP, WiFi SSID, and password
- [ ] Created `.env` file from `.env.example`
- [ ] Transferred values to `.env`
- [ ] Ran `python scripts/apply_env_config.py`
- [ ] Saw success message from script
- [ ] Ran `idf.py build`
- [ ] Ran `idf.py flash`
- [ ] Verified correct IP in serial monitor
- [ ] Tested WebSocket connection
- [ ] Tested WiFi connection
- [ ] Updated team members about new process

---

## 🎉 Migration Complete!

You're now using the environment-based configuration system!

**Benefits you'll immediately notice:**
- ✅ Faster configuration changes
- ✅ No more IP mismatch bugs
- ✅ Easier team collaboration
- ✅ Better security (credentials not in code)

**Next Steps:**
- Read [QUICK_CONFIG_REFERENCE.md](QUICK_CONFIG_REFERENCE.md) for daily usage
- Share `.env.example` with team members
- Enjoy easier configuration! 🚀

---

## 🆘 Need Help?

If you encounter issues during migration:

1. Check the [Troubleshooting section](#common-migration-issues) above
2. Review [CONFIGURATION_GUIDE.md](CONFIGURATION_GUIDE.md)
3. Verify `.env` file format matches `.env.example`
4. Ensure you're running script from correct directory
5. Check serial monitor for actual configuration being used

---

**Migration support added:** As part of configuration system implementation  
**Last updated:** Current session  
**Status:** ✅ Tested and validated
