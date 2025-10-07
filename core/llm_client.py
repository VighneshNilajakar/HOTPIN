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
GROQ_MODEL = "openai/gpt-oss-20b"

# Hotpin system prompt - optimized for TTS and wearable interaction
SYSTEM_PROMPT = """SYSTEM: You are "Hotpin" — a compact, helpful, and privacy-first voice assistant embedded in a wearable device. Behave as a local, utility-focused assistant that speaks simply and clearly. Rules:

1. Purpose & tone
   - Be concise, friendly, and focused. Aim for short sentences (prefer <18 words each) and no long paragraphs.
   - Speak in Indian English. Use neutral, polite phrasing (e.g., "Okay, I will do that" not "Roger").
   - Avoid slang, jokes, and figurative language. Prioritize clarity for reliable TTS playback.

2. Response length & structure
   - Keep answers short: target ~15–60 words for normal replies. If a longer explanation is needed, offer a one-sentence summary first and then a numbered list (max 3 items).
   - If giving steps, number them (1., 2., 3.) and keep each step ≤ 12 words.

3. Context & memory
   - Use only the provided session history (server sends last N turns). Do NOT assume external facts beyond the history.
   - Never invent persistent user data or make claims about stored personal info.

4. Interaction behavior
   - If user intent is a command (e.g., "capture", "play", "stop"), reply with a short explicit action confirmation: e.g., "Capturing image now." or "Recording stopped."
   - If asked for clarification, ask one simple question max. Prefer to act when possible.

5. Output format & TTS friendliness
   - Return plain text only (no JSON or markup). Avoid special characters (¶ • — etc.).
   - Keep punctuation minimal and avoid parentheses and long quotations.
   - If you want the device to play a short beep or sound after an action, include the token: `[BEEP]` on its own line after the confirmation text.

6. Safety & refusal
   - If asked for medical, legal, or dangerous instructions, refuse briefly: "I can't help with that. Please consult a qualified professional."
   - For unknown queries, say: "I don't know that. Would you like me to search?" (do not fabricate answers).

7. Determinism & creativity
   - Use a low randomness setting (temperature ≈ 0.2) to keep answers predictable.
   - Prefer direct answers; avoid creative hypotheticals.

8. Error handling
   - If system needs more time, reply: "Working on it — I'll tell you when done." (Keep short.)"""


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
    print(f"✓ Groq AsyncClient initialized with model: {GROQ_MODEL}")


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


async def get_llm_response(session_id: str, transcript: str) -> str:
    """
    Get LLM response from Groq API with conversation context.
    
    Args:
        session_id: Unique session identifier
        transcript: User's transcribed speech input
    
    Returns:
        str: LLM-generated response text
    
    Raises:
        Exception: If API call fails or client not initialized
    """
    global groq_client
    
    if not groq_client:
        raise RuntimeError("Groq client not initialized. Call init_client() first.")
    
    # Add user message to context
    manage_context(session_id, "user", transcript)
    
    # Retrieve conversation history
    history = SESSION_CONTEXTS[session_id]["history"]
    
    # Construct API payload with system prompt + conversation history
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT}
    ] + history
    
    payload = {
        "model": GROQ_MODEL,
        "messages": messages,
        "temperature": 0.2,  # Low temperature for deterministic responses
        "max_tokens": 200,   # Enforce brevity (15-60 words target)
        "top_p": 0.9
    }
    
    try:
        # Make async API call to Groq
        response = await groq_client.post(
            "/chat/completions",
            json=payload
        )
        response.raise_for_status()
        
        # Parse response
        response_data = response.json()
        assistant_message = response_data["choices"][0]["message"]["content"]
        
        # Add assistant response to context
        manage_context(session_id, "assistant", assistant_message)
        
        return assistant_message
    
    except httpx.HTTPStatusError as e:
        print(f"✗ Groq API HTTP error: {e.response.status_code} - {e.response.text}")
        return "Service temporarily unavailable. Please try again."
    
    except httpx.RequestError as e:
        print(f"✗ Groq API request error: {e}")
        return "Connection error. Please check your network."
    
    except Exception as e:
        print(f"✗ Unexpected error in LLM call: {e}")
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
