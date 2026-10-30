#ifndef JOYEER_COMPRESSION_NATIVE_H
#define JOYEER_COMPRESSION_NATIVE_H

#include "runtime/native.h"

#ifdef __cplusplus
extern "C" {
#endif

int64_t joyeer_compression_create_zip(const JoyeerString* names, int64_t nameCount,
    const JoyeerArray* contents, int64_t contentCount, int64_t method,
    int64_t maximumBytes, JoyeerArray* result);
int64_t joyeer_compression_list_zip(const uint8_t* data, int64_t count,
    int64_t maximumEntries, int64_t maximumNameBytes, JoyeerArray* result);
int64_t joyeer_compression_entries_zip(const uint8_t* data, int64_t count,
    int64_t maximumEntries, int64_t maximumNameBytes, JoyeerArray* names, JoyeerArray* table);
int64_t joyeer_compression_read_entry(const uint8_t* data, int64_t count, int64_t headerOffset,
    int64_t method, int64_t flags, int64_t compressedSize, int64_t size, int64_t checksum,
    int64_t maximumBytes, JoyeerArray* result);
int64_t joyeer_compression_extract_zip(const uint8_t* data, int64_t count,
    int64_t index, int64_t maximumBytes, JoyeerArray* result);

#ifdef __cplusplus
}
#endif

#endif
