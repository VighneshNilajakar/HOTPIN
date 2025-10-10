#!/usr/bin/env python3
"""
Verification Script for I²S/Camera Fixes
Checks if all critical fixes are properly implemented in the codebase
"""

import os
import re
from pathlib import Path

# ANSI color codes
GREEN = '\033[92m'
RED = '\033[91m'
YELLOW = '\033[93m'
RESET = '\033[0m'

def check_file_exists(filepath):
    """Check if file exists"""
    return os.path.isfile(filepath)

def check_mclk_disabled(audio_driver_path):
    """Verify MCLK is disabled in audio driver"""
    print("\n[CHECK 1] Verifying MCLK disabled in audio_driver.c...")
    
    if not check_file_exists(audio_driver_path):
        print(f"  {RED}✗ FAIL{RESET}: File not found: {audio_driver_path}")
        return False
    
    with open(audio_driver_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Check for correct MCLK configuration
    if '.mck_io_num = I2S_PIN_NO_CHANGE' in content or '.mck_io_num=I2S_PIN_NO_CHANGE' in content:
        print(f"  {GREEN}✓ PASS{RESET}: MCLK disabled (I2S_PIN_NO_CHANGE found)")
        return True
    elif 'mck_io_num' in content and 'GPIO_NUM' in content:
        print(f"  {RED}✗ FAIL{RESET}: MCLK appears to be set to a GPIO pin")
        print(f"  {YELLOW}FIX{RESET}: Change to '.mck_io_num = I2S_PIN_NO_CHANGE'")
        return False
    else:
        print(f"  {YELLOW}⚠ WARNING{RESET}: Could not find mck_io_num configuration")
        return False

def check_gpio_isr_guards(button_handler_path, camera_controller_path):
    """Verify GPIO ISR service installation is guarded"""
    print("\n[CHECK 2] Verifying GPIO ISR service guards...")
    
    all_pass = True
    
    # Check button_handler.c
    if not check_file_exists(button_handler_path):
        print(f"  {RED}✗ FAIL{RESET}: File not found: {button_handler_path}")
        all_pass = False
    else:
        with open(button_handler_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        if 'ESP_ERR_INVALID_STATE' in content and 'gpio_install_isr_service' in content:
            print(f"  {GREEN}✓ PASS{RESET}: button_handler.c has ISR guard")
        else:
            print(f"  {RED}✗ FAIL{RESET}: button_handler.c missing ISR guard")
            all_pass = False
    
    # Check camera_controller.c
    if not check_file_exists(camera_controller_path):
        print(f"  {RED}✗ FAIL{RESET}: File not found: {camera_controller_path}")
        all_pass = False
    else:
        with open(camera_controller_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        if 'ESP_ERR_INVALID_STATE' in content and 'gpio_install_isr_service' in content:
            print(f"  {GREEN}✓ PASS{RESET}: camera_controller.c has ISR guard")
        else:
            print(f"  {RED}✗ FAIL{RESET}: camera_controller.c missing ISR guard")
            all_pass = False
    
    return all_pass

def check_safe_transitions(state_manager_path):
    """Verify safe state transition implementation"""
    print("\n[CHECK 3] Verifying safe I²S/Camera transitions...")
    
    if not check_file_exists(state_manager_path):
        print(f"  {RED}✗ FAIL{RESET}: File not found: {state_manager_path}")
        return False
    
    with open(state_manager_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    checks = {
        'stt_pipeline_stop': False,
        'audio_driver_deinit': False,
        'vTaskDelay': False,
        'camera_controller_init': False,
        'camera_controller_deinit': False,
        'audio_driver_init': False,
    }
    
    for key in checks.keys():
        if key in content:
            checks[key] = True
    
    all_found = all(checks.values())
    
    if all_found:
        print(f"  {GREEN}✓ PASS{RESET}: All critical functions present")
        print(f"    - Task stop/start: ✓")
        print(f"    - Driver deinit/init: ✓")
        print(f"    - Hardware delays: ✓")
        return True
    else:
        print(f"  {RED}✗ FAIL{RESET}: Missing critical functions:")
        for key, found in checks.items():
            if not found:
                print(f"    - {key}: {RED}MISSING{RESET}")
        return False

def check_full_duplex_mode(audio_driver_path):
    """Verify I²S is configured in full-duplex mode"""
    print("\n[CHECK 4] Verifying I²S full-duplex configuration...")
    
    if not check_file_exists(audio_driver_path):
        print(f"  {RED}✗ FAIL{RESET}: File not found: {audio_driver_path}")
        return False
    
    with open(audio_driver_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Check for full-duplex mode
    if 'I2S_MODE_TX | I2S_MODE_RX' in content or 'I2S_MODE_TX|I2S_MODE_RX' in content:
        print(f"  {GREEN}✓ PASS{RESET}: Full-duplex mode configured (TX | RX)")
        return True
    elif 'I2S_MODE_TX' in content and 'I2S_MODE_RX' in content:
        print(f"  {YELLOW}⚠ WARNING{RESET}: Both TX and RX modes present but may not be combined")
        return False
    else:
        print(f"  {RED}✗ FAIL{RESET}: Full-duplex mode not detected")
        return False

def check_mutex_usage(state_manager_path):
    """Verify mutex is used for I²S configuration critical sections"""
    print("\n[CHECK 5] Verifying mutex protection...")
    
    if not check_file_exists(state_manager_path):
        print(f"  {RED}✗ FAIL{RESET}: File not found: {state_manager_path}")
        return False
    
    with open(state_manager_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    if 'xSemaphoreTake' in content and 'xSemaphoreGive' in content and 'i2s_config_mutex' in content:
        print(f"  {GREEN}✓ PASS{RESET}: Mutex protection implemented")
        return True
    else:
        print(f"  {YELLOW}⚠ WARNING{RESET}: Mutex protection not clearly visible")
        return False

def main():
    print("=" * 60)
    print("I²S/Camera Fix Verification Script")
    print("=" * 60)
    
    # Define paths (relative to hotpin_esp32_firmware directory)
    base_path = Path(__file__).parent
    audio_driver = base_path / "main" / "audio_driver.c"
    button_handler = base_path / "main" / "button_handler.c"
    camera_controller = base_path / "main" / "camera_controller.c"
    state_manager = base_path / "main" / "state_manager.c"
    
    results = []
    
    # Run all checks
    results.append(("MCLK Disabled", check_mclk_disabled(audio_driver)))
    results.append(("GPIO ISR Guards", check_gpio_isr_guards(button_handler, camera_controller)))
    results.append(("Safe Transitions", check_safe_transitions(state_manager)))
    results.append(("Full-Duplex Mode", check_full_duplex_mode(audio_driver)))
    results.append(("Mutex Protection", check_mutex_usage(state_manager)))
    
    # Summary
    print("\n" + "=" * 60)
    print("VERIFICATION SUMMARY")
    print("=" * 60)
    
    passed = sum(1 for _, result in results if result)
    total = len(results)
    
    for name, result in results:
        status = f"{GREEN}✓ PASS{RESET}" if result else f"{RED}✗ FAIL{RESET}"
        print(f"  {name:.<40} {status}")
    
    print("\n" + "=" * 60)
    if passed == total:
        print(f"{GREEN}✓ ALL CHECKS PASSED ({passed}/{total}){RESET}")
        print("System is ready for testing!")
        return 0
    else:
        print(f"{RED}✗ SOME CHECKS FAILED ({passed}/{total}){RESET}")
        print("Please review failed checks and apply fixes.")
        return 1

if __name__ == "__main__":
    exit(main())
