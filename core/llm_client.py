"""
LLM Client Module - Async Groq API Integration with Context Management
Handles conversation state and low-latency LLM inference using httpx.AsyncClient
"""

import os
import time
import httpx
import json
from typing import Dict, List, Optional
from dotenv import load_dotenv

# Load environment variables
load_dotenv()

# Global session context storage - In-memory dictionary for prototype
# NOTE: This is process-unsafe with multiple Uvicorn workers
# Production deployment requires Redis/PostgreSQL for shared state
SESSION_CONTEXTS: Dict[str, dict] = {}

# Global Groq async client instance
groq_client: Optional[httpx.AsyncClient] = None

# Groq API configuration
GROQ_API_BASE_URL = "https://api.groq.com/openai/v1"
GROQ_API_KEY = os.getenv("GROQ_API_KEY")
GROQ_MODEL = "meta-llama/llama-4-scout-17b-16e-instruct"

# Hotpin system prompt - optimized for TTS, wearable interaction, and vision
SYSTEM_PROMPT = """SYSTEM: You are "Hotpin" — a compact, helpful, and privacy-first voice assistant with vision capabilities. Your goal is to provide short, one-liner answers. Rules:

1.  **MUST BE A SINGLE SENTENCE:** Your entire response must be a single, short sentence.
2.  **NO FORMATTING:** Do not use any formatting, including newlines, lists, or bold text.
3.  **ONE-LINER:** Your response must be a single line of text.
4.  **VISION INTEGRATION:** When an image is provided, analyze it and incorporate visual insights into your concise response.
5.  **CONTEXT AWARENESS:** Reference what you see in the image naturally within your single-sentence answer."""


def init_client() -> None:
    """
    Initialize the global httpx.AsyncClient for Groq API calls.
    Called during FastAPI startup event.
    Uses connection pooling for efficient resource reuse.
    """
    global groq_client
    
    if not GROQ_API_KEY or GROQ_API_KEY == "your_groq_api_key_here":
        raise ValueError("GROQ_API_KEY not set in .env file. Please add your API key.")
    
    groq_client = httpx.AsyncClient(
        base_url=GROQ_API_BASE_URL,
        headers={
            "Authorization": f"Bearer {GROQ_API_KEY}",
            "Content-Type": "application/json"
        },
        timeout=30.0  # 30 second timeout for LLM calls
    )
    print(f"Groq AsyncClient initialized with model: {GROQ_MODEL}")


async def close_client() -> None:
    """
    Gracefully close the Groq async client.
    Called during FastAPI shutdown event.
    Ensures proper cleanup of connections.
    """
    global groq_client
    if groq_client:
        await groq_client.aclose()
        print("✓ Groq AsyncClient closed")


def manage_context(session_id: str, role: str, content: str, max_history_turns: int = 10) -> None:
    """
    Manage conversation context for a session.
    
    Args:
        session_id: Unique session identifier
        role: Message role ("user" or "assistant")
        content: Message content text
        max_history_turns: Maximum number of conversation turns to keep (default: 10)
    
    Maintains context window by keeping only the last N turns to prevent overflow.
    """
    if session_id not in SESSION_CONTEXTS:
        SESSION_CONTEXTS[session_id] = {
            "history": [],
            "last_activity_ts": time.time()
        }
    
    # Append new message
    SESSION_CONTEXTS[session_id]["history"].append({
        "role": role,
        "content": content
    })
    
    # Update timestamp
    SESSION_CONTEXTS[session_id]["last_activity_ts"] = time.time()
    
    # Enforce context window limit (keep last N turns)
    # Each turn = user + assistant message pair
    max_messages = max_history_turns * 2  # user + assistant per turn
    if len(SESSION_CONTEXTS[session_id]["history"]) > max_messages:
        # Keep only the most recent messages
        SESSION_CONTEXTS[session_id]["history"] = SESSION_CONTEXTS[session_id]["history"][-max_messages:]


async def get_llm_response(session_id: str, transcript: str, image_base64: Optional[str] = None) -> str:
    """
    Get LLM response from Groq API with conversation context and optional image.
    
    Args:
        session_id: Unique session identifier
        transcript: User's transcribed speech input
        image_base64: Optional base64-encoded JPEG image for multimodal context
    
    Returns:
        str: LLM-generated response text
    
    Raises:
        Exception: If API call fails or client not initialized
    """
    global groq_client
    
    if not groq_client:
        raise RuntimeError("Groq client not initialized. Call init_client() first.")
    
    # Add user message to context (store as text for history tracking)
    manage_context(session_id, "user", transcript)
    
    # Retrieve conversation history
    history = SESSION_CONTEXTS[session_id]["history"]
    
    # Construct messages with system prompt
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT}
    ]
    
    # Add conversation history, but skip the last user message (we'll add it with image if needed)
    if len(history) > 1:
        messages.extend(history[:-1])
    
    # Construct the current user message with optional image
    if image_base64:
        # Multimodal message format with text and image
        user_message_content = [
            {
                "type": "text",
                "text": transcript
            },
            {
                "type": "image_url",
                "image_url": {
                    "url": f"data:image/jpeg;base64,{image_base64}"
                }
            }
        ]
        print(f"🖼️ [{session_id}] Including image in LLM context (base64 length: {len(image_base64)})")
    else:
        # Text-only message - use simple string format
        user_message_content = transcript
    
    # Add current user message (multimodal or text-only)
    messages.append({
        "role": "user",
        "content": user_message_content
    })
    
    payload = {
        "model": GROQ_MODEL,
        "messages": messages,
        "temperature": 0.2,  # Low temperature for deterministic responses
        "max_tokens": 100,   # Enforce brevity (15-60 words target)
        "top_p": 0.9
    }
    
    # Debug logging to verify payload structure
    if image_base64:
        print(f"🔍 [{session_id}] Sending multimodal request to Groq API")
        print(f"   Model: {GROQ_MODEL}")
        print(f"   Messages count: {len(messages)}")
        print(f"   Last message has image: {isinstance(messages[-1]['content'], list)}")
    
    try:
        # Make async API call to Groq
        response = await groq_client.post(
            "/chat/completions",
            json=payload
        )
        response.raise_for_status()
        
        # Parse response
        response_data = response.json()
        
        # Validate API response structure
        if "choices" not in response_data or len(response_data["choices"]) == 0:
            print(f"✗ Groq API returned malformed response: {response_data}")
            return "I encountered an issue processing your request. Please try again."
        
        assistant_message = response_data["choices"][0]["message"]["content"]
        
        # Validate response content
        if not assistant_message or assistant_message.strip() == "":
            print(f"⚠ Groq API returned empty response for session {session_id}")
            print(f"   Transcript: \"{transcript}\"")
            print(f"   Response data: {response_data}")
            return "I'm having trouble responding right now. Please rephrase your question."
        
        # Add assistant response to context
        manage_context(session_id, "assistant", assistant_message)
        
        return assistant_message
    
    except httpx.HTTPStatusError as e:
        print(f"✗ Groq API HTTP error: {e.response.status_code} - {e.response.text}")
        return "Service temporarily unavailable. Please try again."
    
    except httpx.RequestError as e:
        print(f"✗ Groq API request error: {e}")
        return "Connection error. Please check your network."
    
    except KeyError as e:
        print(f"✗ Groq API response parsing error: Missing key {e}")
        return "I encountered an issue processing your request. Please try again."
    
    except Exception as e:
        print(f"✗ Unexpected error in LLM call: {type(e).__name__}: {e}")
        import traceback
        print(traceback.format_exc())
        return "An error occurred. Please try again."


def get_session_context(session_id: str) -> Optional[dict]:
    """
    Retrieve session context for debugging or monitoring.
    
    Args:
        session_id: Unique session identifier
    
    Returns:
        dict or None: Session context with history and metadata
    """
    return SESSION_CONTEXTS.get(session_id)


def clear_session_context(session_id: str) -> None:
    """
    Clear conversation context for a session.
    Called when WebSocket disconnects.
    
    Args:
        session_id: Unique session identifier
    """
    if session_id in SESSION_CONTEXTS:
        del SESSION_CONTEXTS[session_id]
        print(f"✓ Cleared context for session: {session_id}")
