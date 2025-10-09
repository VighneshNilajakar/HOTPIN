#!/usr/bin/env python3
"""
Automatic Server IP Detection and Configuration Updater
Detects the host PC's IP address and updates the ESP32 firmware .env file
"""

import os
import socket
import subprocess
import sys
from pathlib import Path


def get_local_ip():
    """
    Get the local IP address of this machine.
    Uses socket connection test to determine the active network interface IP.
    """
    try:
        # Create a socket and connect to a public DNS server
        # This doesn't actually send data, just determines which interface would be used
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.1)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
        return local_ip
    except Exception as e:
        print(f"⚠️  Socket method failed: {e}")
        
        # Fallback method
        try:
            hostname = socket.gethostname()
            local_ip = socket.gethostbyname(hostname)
            return local_ip
        except Exception as e:
            print(f"❌ Fallback method failed: {e}")
            return None


def get_wifi_ssid_windows():
    """
    Get the currently connected WiFi SSID on Windows.
    """
    try:
        result = subprocess.run(
            ['netsh', 'wlan', 'show', 'interfaces'],
            capture_output=True,
            text=True,
            timeout=5
        )
        
        if result.returncode == 0:
            for line in result.stdout.split('\n'):
                if 'SSID' in line and 'BSSID' not in line:
                    ssid = line.split(':', 1)[1].strip()
                    return ssid
    except Exception as e:
        print(f"⚠️  Could not detect WiFi SSID: {e}")
    
    return None


def update_env_file(env_path, server_ip, wifi_ssid=None):
    """
    Update the .env file with new server IP and optionally WiFi SSID.
    
    Args:
        env_path: Path to .env file
        server_ip: IP address to set
        wifi_ssid: WiFi SSID to set (optional)
    """
    if not os.path.exists(env_path):
        print(f"❌ .env file not found: {env_path}")
        return False
    
    # Read existing .env file
    with open(env_path, 'r') as f:
        lines = f.readlines()
    
    # Update lines
    updated_lines = []
    ip_updated = False
    ssid_updated = False
    
    for line in lines:
        # Update server IP
        if line.strip().startswith('HOTPIN_SERVER_IP='):
            updated_lines.append(f'HOTPIN_SERVER_IP={server_ip}\n')
            ip_updated = True
            print(f'  ✓ Updated HOTPIN_SERVER_IP={server_ip}')
        
        # Update WiFi SSID if provided
        elif wifi_ssid and line.strip().startswith('HOTPIN_WIFI_SSID='):
            updated_lines.append(f'HOTPIN_WIFI_SSID={wifi_ssid}\n')
            ssid_updated = True
            print(f'  ✓ Updated HOTPIN_WIFI_SSID={wifi_ssid}')
        
        else:
            updated_lines.append(line)
    
    # Write back to file
    with open(env_path, 'w') as f:
        f.writelines(updated_lines)
    
    if ip_updated:
        print(f"✅ Successfully updated .env file")
        return True
    else:
        print(f"⚠️  HOTPIN_SERVER_IP not found in .env file")
        return False


def main():
    print("=" * 60)
    print("🔍 Automatic Server IP Detection & Configuration")
    print("=" * 60)
    print()
    
    # Detect local IP
    print("📡 Detecting local IP address...")
    local_ip = get_local_ip()
    
    if not local_ip:
        print("❌ Failed to detect local IP address")
        print("   Please check your network connection and try again")
        sys.exit(1)
    
    print(f"✓ Detected IP: {local_ip}")
    print()
    
    # Detect WiFi SSID (Windows only)
    wifi_ssid = None
    if os.name == 'nt':  # Windows
        print("📶 Detecting WiFi network...")
        wifi_ssid = get_wifi_ssid_windows()
        if wifi_ssid:
            print(f"✓ Connected to: {wifi_ssid}")
        else:
            print("⚠️  Could not detect WiFi SSID (may be using Ethernet)")
        print()
    
    # Locate .env file
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    env_path = project_root / '.env'
    
    print(f"📝 Updating configuration file...")
    print(f"   File: {env_path}")
    print()
    
    # Update .env file
    success = update_env_file(str(env_path), local_ip, wifi_ssid)
    
    if success:
        print()
        print("=" * 60)
        print("✅ Configuration Updated Successfully!")
        print("=" * 60)
        print()
        print("📋 Summary:")
        print(f"   Server IP: {local_ip}")
        if wifi_ssid:
            print(f"   WiFi Network: {wifi_ssid}")
        print()
        print("🔧 Next steps:")
        print("   1. Verify WiFi password in .env file")
        print("   2. Run: python scripts/apply_env_config.py")
        print("   3. Run: idf.py build flash monitor")
        print()
    else:
        print()
        print("❌ Configuration update failed")
        sys.exit(1)


if __name__ == "__main__":
    main()
