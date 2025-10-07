/**
 * @file json_protocol.h
 * @brief JSON message formatting for WebSocket protocol
 * 
 * Provides helpers to build protocol-compliant JSON messages:
 * - start: {"type":"start","session":"id","sampleRate":16000,"channels":1}
 * - end: {"type":"end","session":"id"}
 */

#ifndef JSON_PROTOCOL_H
#define JSON_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Build "start" JSON message for STT streaming
 * 
 * Format: {"type":"start","session":"<session_id>","sampleRate":16000,"channels":1}
 * 
 * @param session_id Session identifier string
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of bytes written (excluding null terminator), or -1 on error
 */
int json_protocol_build_start(const char *session_id, char *buffer, size_t buffer_size);

/**
 * @brief Build "end" JSON message for STT stream termination
 * 
 * Format: {"type":"end","session":"<session_id>"}
 * 
 * @param session_id Session identifier string
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of bytes written (excluding null terminator), or -1 on error
 */
int json_protocol_build_end(const char *session_id, char *buffer, size_t buffer_size);

/**
 * @brief Generate unique session ID
 * 
 * Format: "hotpin-<MAC_address_suffix>-<timestamp>"
 * 
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of bytes written (excluding null terminator), or -1 on error
 */
int json_protocol_generate_session_id(char *buffer, size_t buffer_size);

#endif // JSON_PROTOCOL_H
