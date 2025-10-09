# 🎯 Quick Reference: Network Configuration

## 🚀 Change Server IP (Easiest Method)

```bash
# 1. Edit .env file
nano .env

# 2. Update this line:
HOTPIN_SERVER_IP=10.240.253.58  # ← Change to your server IP

# 3. Apply changes
python scripts/apply_env_config.py

# 4. Rebuild and flash
idf.py build
idf.py -p COM5 flash  # Replace COM5 with your port
```

**That's it!** Both WebSocket and HTTP will use the new IP automatically.

---

## 📋 Common Scenarios

### Scenario 1: Server IP Changed
```bash
# Edit .env
HOTPIN_SERVER_IP=192.168.1.150  # ← New IP

# Apply and deploy
python scripts/apply_env_config.py
idf.py build flash
```

### Scenario 2: Different WiFi Network
```bash
# Edit .env
HOTPIN_WIFI_SSID=MyHomeNetwork
HOTPIN_WIFI_PASSWORD=mypassword123

# Apply and deploy
python scripts/apply_env_config.py
idf.py build flash
```

### Scenario 3: Multiple ESP32 Devices
```bash
# Edit .env for each device
HOTPIN_SESSION_ID=esp32-cam-kitchen   # Device 1
# or
HOTPIN_SESSION_ID=esp32-cam-bedroom   # Device 2

# Apply and deploy to each device
python scripts/apply_env_config.py
idf.py -p COM5 build flash  # Device 1
idf.py -p COM6 build flash  # Device 2
```

---

## 🔍 Verify Configuration

After flashing, check serial monitor output:

```
✅ Expected:
I (xxx) WEBSOCKET: Server URI: ws://YOUR_IP:8000/ws
I (xxx) HTTP_CLIENT: HTTP client initialized (server: http://YOUR_IP:8000)
I (xxx) WEBSOCKET: ✅ WebSocket connected to server

❌ If you see different IP:
- You forgot to rebuild after changing .env
- Run: python scripts/apply_env_config.py
- Then: idf.py build flash
```

---

## 📦 Files You Should Edit

| File | Purpose | Commit to Git? |
|------|---------|----------------|
| `.env` | Your configuration | ❌ No (sensitive) |
| `.env.example` | Template for team | ✅ Yes |
| `sdkconfig` | Auto-generated | ❌ No |

---

## 🆘 Quick Troubleshooting

| Problem | Solution |
|---------|----------|
| Python script not found | Run from `hotpin_esp32_firmware/` directory |
| Changes not applied | Run `idf.py build` after script |
| Wrong IP in serial monitor | Check `.env` file has correct values |
| Can't connect to server | Verify server is running on that IP/port |

---

## 💡 Pro Tips

1. **Always test after changes:** Use serial monitor to verify
2. **Keep .env.example updated:** Document all required variables
3. **Use version control:** Commit `.env.example`, ignore `.env`
4. **Server IP finder:** Run `ipconfig` (Windows) or `ifconfig` (Linux/Mac)

---

For detailed guide, see: [CONFIGURATION_GUIDE.md](CONFIGURATION_GUIDE.md)
