#!/usr/bin/env python3
"""
Apply .env configuration to sdkconfig file
This script reads values from .env and updates the sdkconfig file
"""

import os
import sys
import re
from pathlib import Path

def load_env_file(env_path):
    """Load environment variables from .env file"""
    env_vars = {}
    
    if not os.path.exists(env_path):
        print(f"❌ Error: {env_path} not found!")
        print(f"💡 Tip: Copy .env.example to .env and edit the values")
        return None
    
    print(f"📖 Reading configuration from: {env_path}")
    
    with open(env_path, 'r', encoding='utf-8') as f:
        for line_num, line in enumerate(f, 1):
            line = line.strip()
            
            # Skip comments and empty lines
            if not line or line.startswith('#'):
                continue
            
            # Parse KEY=VALUE
            if '=' in line:
                key, value = line.split('=', 1)
                key = key.strip()
                value = value.strip()
                
                # Remove quotes if present
                if value.startswith('"') and value.endswith('"'):
                    value = value[1:-1]
                elif value.startswith("'") and value.endswith("'"):
                    value = value[1:-1]
                
                env_vars[key] = value
                print(f"  ✓ {key}={value}")
    
    return env_vars

def update_sdkconfig(sdkconfig_path, env_vars):
    """Update sdkconfig file with values from .env"""
    
    if not os.path.exists(sdkconfig_path):
        print(f"❌ Error: {sdkconfig_path} not found!")
        print(f"💡 Tip: Run 'idf.py menuconfig' first to generate sdkconfig")
        return False
    
    # Mapping between .env keys and sdkconfig keys
    mapping = {
        'HOTPIN_SERVER_IP': 'CONFIG_HOTPIN_SERVER_IP',
        'HOTPIN_SERVER_PORT': 'CONFIG_HOTPIN_SERVER_PORT',
        'HOTPIN_WIFI_SSID': 'CONFIG_HOTPIN_WIFI_SSID',
        'HOTPIN_WIFI_PASSWORD': 'CONFIG_HOTPIN_WIFI_PASSWORD',
        'HOTPIN_SESSION_ID': 'CONFIG_HOTPIN_SESSION_ID',
        'HOTPIN_AUTH_TOKEN': 'CONFIG_HOTPIN_AUTH_TOKEN'
    }
    
    print(f"\n📝 Updating: {sdkconfig_path}")
    
    # Read current sdkconfig
    with open(sdkconfig_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()
    
    # Update values
    updated_count = 0
    for i, line in enumerate(lines):
        for env_key, config_key in mapping.items():
            if env_key not in env_vars:
                continue
            
            # Match CONFIG_KEY=value or CONFIG_KEY="value"
            pattern = f'^{config_key}='
            if re.match(pattern, line):
                value = env_vars[env_key]
                
                # Determine if value should be quoted
                if config_key.endswith('_PORT'):
                    # Port is a number
                    new_line = f'{config_key}={value}\n'
                else:
                    # Everything else is a string
                    new_line = f'{config_key}="{value}"\n'
                
                if lines[i] != new_line:
                    lines[i] = new_line
                    updated_count += 1
                    print(f"  ✓ Updated {config_key}")
    
    # Write updated sdkconfig
    with open(sdkconfig_path, 'w', encoding='utf-8') as f:
        f.writelines(lines)
    
    if updated_count > 0:
        print(f"\n✅ Successfully updated {updated_count} configuration(s)")
        print(f"🔨 Next step: Run 'idf.py build' to rebuild with new settings")
    else:
        print(f"\n⚠️  No changes made (values already match)")
    
    return True

def main():
    # Get project root directory (parent of scripts/)
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    
    env_file = project_root / '.env'
    sdkconfig_file = project_root / 'sdkconfig'
    
    print("=" * 60)
    print("🔧 HotPin ESP32-CAM Configuration Updater")
    print("=" * 60)
    
    # Load .env
    env_vars = load_env_file(env_file)
    if env_vars is None:
        return 1
    
    # Update sdkconfig
    if not update_sdkconfig(sdkconfig_file, env_vars):
        return 1
    
    print("\n" + "=" * 60)
    print("✅ Configuration update complete!")
    print("=" * 60)
    print("\n⚠️  IMPORTANT: Configuration changes require firmware rebuild!")
    print("🔨 Run the following command to apply changes:")
    print("   cd hotpin_esp32_firmware && idf.py build flash monitor")
    print("=" * 60)
    
    return 0

if __name__ == '__main__':
    sys.exit(main())
