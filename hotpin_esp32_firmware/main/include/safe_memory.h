/**
 * @file safe_memory.h
 * @brief Safe memory management utilities for HotPin ESP32-CAM firmware
 * 
 * Provides safer alternatives to standard memory allocation/deallocation functions
 * with additional checks to prevent common memory-related crashes like double-free
 * and invalid pointer access.
 */

#ifndef SAFE_MEMORY_H
#define SAFE_MEMORY_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Safely free memory and set pointer to NULL
 * @param ptr Pointer to pointer to be freed
 * @return true if memory was freed, false if it was already NULL
 */
static inline bool safe_free(void **ptr) {
    if (ptr == NULL || *ptr == NULL) {
        return false;
    }
    
    heap_caps_free(*ptr);
    *ptr = NULL;
    return true;
}

/**
 * @brief Safely allocate memory with zero initialization
 * @param size Size of memory to allocate
 * @param caps Memory capabilities flags (e.g., MALLOC_CAP_SPIRAM, MALLOC_CAP_DMA)
 * @return Pointer to allocated memory, or NULL on failure
 */
static inline void *safe_calloc(size_t num, size_t size, uint32_t caps) {
    // Check for potential overflow
    if (num > 0 && size > SIZE_MAX / num) {
        ESP_LOGE("SAFE_MEM", "calloc size overflow detected");
        return NULL;
    }
    
    void *ptr = heap_caps_calloc(num, size, caps);
    return ptr;
}

/**
 * @brief Safe string duplicate function with NULL check
 * @param src Source string to duplicate
 * @return Pointer to duplicated string, or NULL on failure
 */
static inline char *safe_strdup(const char *src) {
    if (src == NULL) {
        return NULL;
    }
    
    size_t len = strlen(src);
    char *dst = heap_caps_malloc(len + 1, MALLOC_CAP_DEFAULT);
    if (dst != NULL) {
        strcpy(dst, src);
    }
    return dst;
}

/**
 * @brief Check if a pointer is valid (within known memory regions)
 * @param ptr Pointer to validate
 * @return true if pointer is likely valid, false otherwise
 */
static inline bool is_valid_ptr(const void *ptr) {
    if (ptr == NULL) {
        return false;
    }
    
    uintptr_t addr = (uintptr_t)ptr;
    
    // Check if in internal RAM range (rough check)
    if (addr >= 0x3FF00000 && addr < 0x40000000) {
        return true;
    }
    
    // Check if in PSRAM range (rough check)
    if (addr >= 0x3F800000 && addr < 0x3FC00000) {
        return true;
    }
    
    return false;
}

#ifdef __cplusplus
}
#endif

#endif // SAFE_MEMORY_H