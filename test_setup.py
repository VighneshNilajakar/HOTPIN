"""
Setup Verification Script
Tests all components before running the full server
"""

import os
import sys
from pathlib import Path

def test_environment_variables():
    """Test if .env file is configured"""
    print("\n1. Testing Environment Variables...")
    
    from dotenv import load_dotenv
    load_dotenv()
    
    groq_key = os.getenv("GROQ_API_KEY")
    model_path = os.getenv("VOSK_MODEL_PATH")
    
    if not groq_key or groq_key == "your_groq_api_key_here":
        print("   ✗ GROQ_API_KEY not set in .env")
        print("   → Please add your Groq API key to the .env file")
        return False
    
    print(f"   ✓ GROQ_API_KEY configured")
    print(f"   ✓ VOSK_MODEL_PATH: {model_path}")
    return True


def test_vosk_model():
    """Test if Vosk model exists and is valid"""
    print("\n2. Testing Vosk Model...")
    
    from dotenv import load_dotenv
    load_dotenv()
    
    model_path = os.getenv("VOSK_MODEL_PATH", "./model")
    
    if not os.path.exists(model_path):
        print(f"   ✗ Model directory not found: {model_path}")
        print("   → Please download a Vosk model and place it in the model/ directory")
        print("   → Download from: https://alphacephei.com/vosk/models")
        return False
    
    # Check for essential model files
    required_dirs = ["am", "conf", "graph"]
    missing = []
    
    for dir_name in required_dirs:
        dir_path = os.path.join(model_path, dir_name)
        if not os.path.exists(dir_path):
            missing.append(dir_name)
    
    if missing:
        print(f"   ⚠ Model directory incomplete, missing: {', '.join(missing)}")
        print("   → The model may be corrupted or incomplete")
        return False
    
    print(f"   ✓ Vosk model found at: {model_path}")
    print(f"   ✓ Model structure verified (am, conf, graph)")
    
    # Try to load the model
    try:
        from vosk import Model
        print("   → Loading model (this may take a moment)...")
        model = Model(model_path)
        print("   ✓ Model loaded successfully!")
        return True
    except Exception as e:
        print(f"   ✗ Failed to load model: {e}")
        return False


def test_pyttsx3():
    """Test if pyttsx3 TTS engine works"""
    print("\n3. Testing pyttsx3 TTS Engine...")
    
    try:
        import pyttsx3
        engine = pyttsx3.init()
        
        # Get available voices
        voices = engine.getProperty('voices')
        print(f"   ✓ pyttsx3 initialized successfully")
        print(f"   ✓ Found {len(voices)} voice(s):")
        
        for i, voice in enumerate(voices[:3]):  # Show first 3 voices
            print(f"      - {voice.name}")
        
        if len(voices) > 3:
            print(f"      ... and {len(voices) - 3} more")
        
        engine.stop()
        return True
    
    except Exception as e:
        print(f"   ✗ pyttsx3 test failed: {e}")
        print("   → Windows: SAPI5 should be pre-installed")
        print("   → Linux: Install espeak with 'sudo apt install espeak'")
        print("   → macOS: Should work out-of-the-box")
        return False


def test_groq_connection():
    """Test if Groq API is accessible"""
    print("\n4. Testing Groq API Connection...")
    
    from dotenv import load_dotenv
    import httpx
    
    load_dotenv()
    
    api_key = os.getenv("GROQ_API_KEY")
    
    if not api_key or api_key == "your_groq_api_key_here":
        print("   ⚠ Skipping (API key not configured)")
        return False
    
    try:
        # Simple test request
        client = httpx.Client(
            base_url="https://api.groq.com/openai/v1",
            headers={"Authorization": f"Bearer {api_key}"},
            timeout=10.0
        )
        
        # Test with a simple models list request
        response = client.get("/models")
        
        if response.status_code == 200:
            print("   ✓ Groq API connection successful")
            print("   ✓ API key is valid")
            return True
        else:
            print(f"   ✗ API returned status code: {response.status_code}")
            return False
    
    except Exception as e:
        print(f"   ✗ Failed to connect to Groq API: {e}")
        print("   → Check your internet connection")
        print("   → Verify your API key is correct")
        return False


def test_dependencies():
    """Test if all required packages are installed"""
    print("\n5. Testing Python Dependencies...")
    
    # Map package names to their import names when different
    required_packages = {
        "fastapi": "fastapi",
        "uvicorn": "uvicorn",
        "websockets": "websockets",
        "vosk": "vosk",
        "pyttsx3": "pyttsx3",
        "groq": "groq",
        "httpx": "httpx",
        "pydantic": "pydantic",
        "python-dotenv": "dotenv"  # Import name is different!
    }
    
    missing = []
    
    for package_name, import_name in required_packages.items():
        try:
            __import__(import_name)
            print(f"   ✓ {package_name}")
        except ImportError:
            print(f"   ✗ {package_name}")
            missing.append(package_name)
    
    if missing:
        print(f"\n   Missing packages: {', '.join(missing)}")
        print("   → Run: pip install -r requirements.txt")
        return False
    
    return True


def main():
    """Run all tests"""
    print("="*60)
    print("Hotpin Prototype - Setup Verification")
    print("="*60)
    
    results = {
        "Dependencies": test_dependencies(),
        "Environment Variables": test_environment_variables(),
        "Vosk Model": test_vosk_model(),
        "TTS Engine": test_pyttsx3(),
        "Groq API": test_groq_connection()
    }
    
    print("\n" + "="*60)
    print("Summary:")
    print("="*60)
    
    all_passed = True
    for test_name, passed in results.items():
        status = "✓ PASS" if passed else "✗ FAIL"
        print(f"{status:10} {test_name}")
        if not passed:
            all_passed = False
    
    print("="*60)
    
    if all_passed:
        print("\n🎉 All tests passed! You're ready to run the server.")
        print("\nStart the server with:")
        print("   python main.py")
        print("\nOr for production with multiple workers:")
        print("   uvicorn main:app --host 0.0.0.0 --port 8000 --workers 4")
    else:
        print("\n⚠ Some tests failed. Please fix the issues above before running the server.")
    
    print()


if __name__ == "__main__":
    main()
