#include "host_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

bool joyeer_host_valid_utf8(const uint8_t* data, int64_t count) {
    if (count < 0 || (count != 0 && data == NULL)) return false;
    int64_t index = 0;
    while (index < count) {
        const uint8_t first = data[index++];
        if (first <= 0x7f) continue;
        int remaining;
        uint8_t lower = 0x80;
        uint8_t upper = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            remaining = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            remaining = 2;
            if (first == 0xe0) lower = 0xa0;
            if (first == 0xed) upper = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            remaining = 3;
            if (first == 0xf0) lower = 0x90;
            if (first == 0xf4) upper = 0x8f;
        } else {
            return false;
        }
        if (count - index < remaining) return false;
        if (data[index] < lower || data[index] > upper) return false;
        ++index;
        for (int continuation = 1; continuation < remaining; ++continuation) {
            if (data[index] < 0x80 || data[index] > 0xbf) return false;
            ++index;
        }
    }
    return true;
}

JoyeerHostChar* joyeer_host_string(
        const uint8_t* data, int64_t count, bool allowEmpty, int64_t* errorCode) {
    if (errorCode == NULL) joyeer_panic("invalid host error storage");
    *errorCode = 0;
    if (count < 0 || (count != 0 && data == NULL) ||
        (!allowEmpty && count == 0) ||
        (count > 0 && memchr(data, 0, (size_t)count) != NULL)) {
#if defined(_WIN32)
        *errorCode = ERROR_INVALID_PARAMETER;
#else
        *errorCode = EINVAL;
#endif
        return NULL;
    }
    if (!joyeer_host_valid_utf8(data, count)) {
#if defined(_WIN32)
        *errorCode = ERROR_NO_UNICODE_TRANSLATION;
#else
        *errorCode = EILSEQ;
#endif
        return NULL;
    }
#if defined(_WIN32)
    if (count > INT_MAX) {
        *errorCode = ERROR_INVALID_PARAMETER;
        return NULL;
    }
    const int length = count == 0 ? 0 : MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)data, (int)count, NULL, 0);
    if (count != 0 && length == 0) {
        *errorCode = GetLastError();
        return NULL;
    }
    wchar_t* result = (wchar_t*)malloc(((size_t)length + 1) * sizeof(wchar_t));
    if (result == NULL) joyeer_panic("out of memory");
    if (length != 0 && MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)data, (int)count,
            result, length) != length) {
        *errorCode = GetLastError();
        free(result);
        return NULL;
    }
    result[length] = L'\0';
#else
    if ((uint64_t)count >= SIZE_MAX) {
        *errorCode = EINVAL;
        return NULL;
    }
    char* result = (char*)malloc((size_t)count + 1);
    if (result == NULL) joyeer_panic("out of memory");
    if (count != 0) memcpy(result, data, (size_t)count);
    result[count] = '\0';
#endif
    return result;
}
