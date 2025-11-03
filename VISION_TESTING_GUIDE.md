# Quick Test Guide: Vision-Enabled Hotpin

## Prerequisites
✅ Groq API key with Llama 4 Scout access  
✅ ESP32-CAM with image capture capability  
✅ Server running on network-accessible IP  

## Test Scenario 1: Basic Vision Query

### Step 1: Start Server
```powershell
cd F:\Documents\HOTPIN\HOTPIN
python main.py
```

**Expected Output**:
```
Groq AsyncClient initialized with model: meta-llama/llama-4-scout-17b-16e-instruct
Vosk model loaded successfully
Server started on ws://<IP>:8000/ws
```

### Step 2: Capture Image
Via ESP32-CAM or curl:
```bash
curl -X POST http://<SERVER_IP>:8000/image \
  -F "session=test-001" \
  -F "file=@test_image.jpg"
```

**Expected Response**:
```json
{
  "status": "success",
  "message": "Image received and stored for next voice interaction",
  "context_ready": true
}
```

**Server Log**:
```
📷 [test-001] Image received: test_image.jpg, 45678 bytes (44.61 KB)
🖼️ [test-001] Image stored in session context (base64 size: 60904 chars)
💾 [test-001] Image saved: captured_images/test-001_20251103_143022.jpg
```

### Step 3: Voice Query with Image Context
Connect via WebSocket and send audio with query: **"What do you see in this image?"**

**Expected Server Log**:
```
📝 [test-001] Transcript: "what do you see in this image"
🖼️ [test-001] Using stored image context for LLM request
🖼️ [test-001] Including image in LLM context (base64 length: 60904)
🤖 [test-001] LLM response: "I see a red apple on a wooden table with natural lighting."
🗑️ [test-001] Cleared image context after use
```

**Expected TTS Response**: Audible description of image content

---

## Test Scenario 2: Sequential Queries (No Image Persistence)

### Step 1: Upload Image
```bash
curl -X POST http://<SERVER_IP>:8000/image \
  -F "session=test-002" \
  -F "file=@apple.jpg"
```

### Step 2: First Query (Uses Image)
Voice: **"What color is the object?"**

**Expected**: "The object is red."  
**Server Log**: `🗑️ [test-002] Cleared image context after use`

### Step 3: Second Query (No Image)
Voice: **"How big is it?"**

**Expected**: "I don't have size information without additional context."  
**No Image Log**: Image context not available (cleared after Step 2)

### Step 4: Upload New Image
```bash
curl -X POST http://<SERVER_IP>:8000/image \
  -F "session=test-002" \
  -F "file=@banana.jpg"
```

### Step 5: Third Query (Uses New Image)
Voice: **"What fruit is this?"**

**Expected**: "This is a banana."  
**Verify**: Different image context than Step 2

---

## Test Scenario 3: Text-Only (Regression Test)

### No Image Upload
Skip image capture entirely.

### Voice Query
**"What's the capital of France?"**

**Expected Response**: "The capital of France is Paris."

**Server Log Should NOT Include**:
- ❌ "Using stored image context"
- ❌ "Including image in LLM context"

**Verify**: Text-only queries still work without images.

---

## Test Scenario 4: Multi-Session Isolation

### Session A: Upload Image A
```bash
curl -X POST http://<SERVER_IP>:8000/image \
  -F "session=session-A" \
  -F "file=@cat.jpg"
```

### Session B: Upload Image B
```bash
curl -X POST http://<SERVER_IP>:8000/image \
  -F "session=session-B" \
  -F "file=@dog.jpg"
```

### Session A Query
**"What animal is this?"**

**Expected**: "This is a cat."  
**Verify**: Session A gets cat image, not dog

### Session B Query
**"What animal is this?"**

**Expected**: "This is a dog."  
**Verify**: Session B gets dog image, not cat

---

## Test Scenario 5: Error Handling

### Test 5a: Large Image (>5MB)
```bash
curl -X POST http://<SERVER_IP>:8000/image \
  -F "session=test-error" \
  -F "file=@huge_image.jpg"
```

**Expected**: 
- Server accepts (no size validation yet)
- May cause timeout if >20MB (Groq limit)

### Test 5b: Corrupted Image
Upload non-JPEG file as image:
```bash
curl -X POST http://<SERVER_IP>:8000/image \
  -F "session=test-error" \
  -F "file=@text_file.txt"
```

**Expected**:
- Server stores base64
- LLM may return: "I cannot interpret this image."

### Test 5c: Query Without Audio
Send empty audio buffer after image upload.

**Expected**:
```
⚠ [test-error] Empty transcription
Status: "Could not understand audio. Please try again."
```

**Verify**: Image still stored (not cleared on empty transcript)

---

## Test Scenario 6: Session Cleanup

### Test 6a: Reset Signal
1. Upload image
2. Send WebSocket signal: `{"signal": "RESET"}`

**Expected Server Log**:
```
🗑️ [test-006] Cleared stored image context on reset
🔄 [test-006] Session reset
```

### Test 6b: Disconnect
1. Upload image
2. Close WebSocket connection

**Expected Server Log**:
```
🗑️ [test-006] Cleared stored image context
🧹 [test-006] Session cleaned up
```

---

## Debugging Checklist

### Issue: "Image not found in LLM context"
**Check**:
- [ ] Image successfully uploaded (check `captured_images/` folder)
- [ ] Session ID matches between upload and WebSocket
- [ ] Image not cleared by previous query
- [ ] Server log shows: `🖼️ [...] Image stored in session context`

### Issue: "Empty LLM response"
**Check**:
- [ ] Groq API key valid
- [ ] Model name correct: `meta-llama/llama-4-scout-17b-16e-instruct`
- [ ] Base64 image size reasonable (<4MB encoded)
- [ ] Network connectivity to Groq API

### Issue: "Image persists across queries"
**Expected Behavior**: Images cleared after use  
**If Persisting**:
- [ ] Check cleanup code in WebSocket handler (line 477)
- [ ] Verify `del SESSION_IMAGES[session_id]` executed
- [ ] Check server logs for `🗑️ Cleared image context after use`

### Issue: "Model returns text-only response despite image"
**Check**:
- [ ] Server log shows: `Including image in LLM context`
- [ ] Base64 string not truncated (check length)
- [ ] Image format valid (JPEG preferred)
- [ ] Try explicit query: "Describe what you see in this image"

---

## Expected Performance Metrics

### Latency Breakdown (Typical)
- **Image Upload**: 100-300ms (depends on network)
- **Base64 Encoding**: <10ms (negligible)
- **STT Processing**: 200-500ms (Vosk)
- **LLM Inference**: 150-400ms (Llama 4 Scout on Groq)
- **TTS Synthesis**: 300-800ms (pyttsx3)
- **Total**: ~1-2 seconds (with image)

### Comparison to Text-Only
- **Text-Only**: ~1-1.5 seconds
- **With Image**: ~1.5-2 seconds
- **Overhead**: +200-500ms (acceptable)

---

## Sample Queries to Test Vision

### Object Recognition
- "What do you see?"
- "What object is in this image?"
- "Identify what's in the picture."

### Color Detection
- "What color is this?"
- "What's the main color in this image?"

### Scene Description
- "Describe this scene."
- "What's happening here?"

### Counting
- "How many objects are there?"
- "Count the items in this image."

### OCR (Text Reading)
- "What does the text say?"
- "Read the sign in this image."

### Spatial Reasoning
- "Where is the object located?"
- "What's on the left side of the image?"

---

## Success Criteria

✅ **Text-only queries work** (no regression)  
✅ **Image context included when available**  
✅ **LLM provides visual insights**  
✅ **Images cleared after use** (no stale context)  
✅ **Multi-session isolation** (no cross-contamination)  
✅ **Session cleanup on disconnect/reset**  
✅ **Performance acceptable** (<2s total latency)  
✅ **No memory leaks** (monitor over 30+ queries)  

---

## Troubleshooting Commands

### Check Server Status
```powershell
curl http://<SERVER_IP>:8000/test
```

### View Recent Logs
```powershell
# In PowerShell where server is running
# Press Ctrl+C to stop, scroll up to review logs
```

### Clear All Session Data
```powershell
# Restart server (clears in-memory dictionaries)
# Ctrl+C → python main.py
```

### Test Image Upload Manually
```python
import requests
import base64

with open("test_image.jpg", "rb") as f:
    files = {"file": f}
    data = {"session": "manual-test"}
    response = requests.post("http://localhost:8000/image", files=files, data=data)
    print(response.json())
```

---

**Test Environment**: Windows 11 + Python 3.11 + Groq API  
**Model**: meta-llama/llama-4-scout-17b-16e-instruct  
**Test Date**: November 3, 2025  
