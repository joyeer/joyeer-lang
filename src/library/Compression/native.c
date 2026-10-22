#include "native.h"
#include "runtime/host_internal.h"
#include "miniz.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    COMPRESSION_OK = 0,
    COMPRESSION_INVALID_ARGUMENT = 1,
    COMPRESSION_INVALID_ARCHIVE = 2,
    COMPRESSION_UNSUPPORTED = 3,
    COMPRESSION_LIMIT = 4,
    COMPRESSION_NOT_FOUND = 5,
    COMPRESSION_DUPLICATE = 6,
    COMPRESSION_NATIVE_ERROR = 1000
};

typedef struct OutputBuffer {
    uint8_t* data;
    size_t count;
    size_t capacity;
    size_t limit;
    bool limited;
    bool allocationFailed;
} OutputBuffer;

static bool validView(const void* data, int64_t count, size_t stride) {
    return count >= 0 && (uint64_t)count <= SIZE_MAX / stride &&
            (count == 0 || data != NULL);
}

static int64_t nativeError(mz_zip_archive* zip) {
    const mz_zip_error error = mz_zip_peek_last_error(zip);
    switch (error) {
        case MZ_ZIP_UNSUPPORTED_METHOD:
        case MZ_ZIP_UNSUPPORTED_ENCRYPTION:
        case MZ_ZIP_UNSUPPORTED_FEATURE:
        case MZ_ZIP_UNSUPPORTED_MULTIDISK:
            return COMPRESSION_UNSUPPORTED;
        case MZ_ZIP_TOO_MANY_FILES:
        case MZ_ZIP_FILE_TOO_LARGE:
        case MZ_ZIP_ARCHIVE_TOO_LARGE:
            return COMPRESSION_LIMIT;
        case MZ_ZIP_FAILED_FINDING_CENTRAL_DIR:
        case MZ_ZIP_NOT_AN_ARCHIVE:
        case MZ_ZIP_INVALID_HEADER_OR_CORRUPTED:
        case MZ_ZIP_DECOMPRESSION_FAILED:
        case MZ_ZIP_UNEXPECTED_DECOMPRESSED_SIZE:
        case MZ_ZIP_CRC_CHECK_FAILED:
        case MZ_ZIP_VALIDATION_FAILED:
            return COMPRESSION_INVALID_ARCHIVE;
        default:
            return COMPRESSION_NATIVE_ERROR +
                    (error == MZ_ZIP_NO_ERROR ? MZ_ZIP_INTERNAL_ERROR : error);
    }
}

static size_t writeBuffer(void* opaque, mz_uint64 offset, const void* source, size_t count) {
    OutputBuffer* output = (OutputBuffer*)opaque;
    if (offset > output->limit || count > output->limit - (size_t)offset) {
        output->limited = true;
        return 0;
    }
    const size_t end = (size_t)offset + count;
    if (end > output->capacity) {
        size_t capacity = output->capacity == 0 ? 4096 : output->capacity;
        if (capacity > output->limit) capacity = output->limit;
        while (capacity < end) {
            capacity = capacity > output->limit / 2 ? output->limit : capacity * 2;
        }
        void* grown = realloc(output->data, capacity);
        if (grown == NULL) {
            output->allocationFailed = true;
            return 0;
        }
        output->data = (uint8_t*)grown;
        output->capacity = capacity;
    }
    if ((size_t)offset > output->count) {
        memset(output->data + output->count, 0, (size_t)offset - output->count);
    }
    if (count != 0) memcpy(output->data + (size_t)offset, source, count);
    if (end > output->count) output->count = end;
    return count;
}

static int64_t writeError(mz_zip_archive* zip, const OutputBuffer* output) {
    if (output->limited) return COMPRESSION_LIMIT;
    if (output->allocationFailed) return COMPRESSION_NATIVE_ERROR + MZ_ZIP_ALLOC_FAILED;
    return nativeError(zip);
}

static bool validEntryName(JoyeerString name) {
    if (!validView(name.data, name.count, 1) || name.count == 0 || name.count > UINT16_MAX ||
        !joyeer_host_valid_utf8(name.data, name.count)) return false;
    int64_t start = 0;
    for (int64_t index = 0; index <= name.count; ++index) {
        if (index < name.count) {
            const uint8_t byte = name.data[index];
            if (byte == 0 || byte == '\\' || byte == ':') return false;
            if (byte != '/') continue;
        }
        const int64_t length = index - start;
        if (length == 0 || (length == 1 && name.data[start] == '.') ||
            (length == 2 && name.data[start] == '.' && name.data[start + 1] == '.')) return false;
        start = index + 1;
    }
    return true;
}

int64_t joyeer_compression_create_zip(const JoyeerString* names, int64_t nameCount,
        const JoyeerArray* contents, int64_t contentCount, int64_t method,
        int64_t maximumBytes, JoyeerArray* result) {
    if (result == NULL || maximumBytes < 0 || (method != 0 && method != 6) ||
        nameCount != contentCount || !validView(names, nameCount, sizeof(*names)) ||
        !validView(contents, contentCount, sizeof(*contents))) return COMPRESSION_INVALID_ARGUMENT;
    if (nameCount >= UINT16_MAX) return COMPRESSION_LIMIT;
    for (int64_t index = 0; index < nameCount; ++index) {
        if (!validEntryName(names[index]) || !validView(contents[index].data, contents[index].count, 1)) {
            return COMPRESSION_INVALID_ARGUMENT;
        }
        if ((uint64_t)contents[index].count >= UINT32_MAX) return COMPRESSION_LIMIT;
        for (int64_t previous = 0; previous < index; ++previous) {
            if (names[previous].count == names[index].count &&
                memcmp(names[previous].data, names[index].data, (size_t)names[index].count) == 0) {
                return COMPRESSION_DUPLICATE;
            }
        }
    }
    OutputBuffer output = {0};
    output.limit = (size_t)((uint64_t)maximumBytes < UINT32_MAX ? maximumBytes : UINT32_MAX - 1);
    mz_zip_archive zip = {0};
    zip.m_pWrite = writeBuffer;
    zip.m_pIO_opaque = &output;
    if (!mz_zip_writer_init(&zip, 0)) return nativeError(&zip);
    int64_t status = COMPRESSION_OK;
    for (int64_t index = 0; index < nameCount; ++index) {
        char* name = (char*)malloc((size_t)names[index].count + 1);
        if (name == NULL) {
            status = COMPRESSION_NATIVE_ERROR + MZ_ZIP_ALLOC_FAILED;
            break;
        }
        memcpy(name, names[index].data, (size_t)names[index].count);
        name[names[index].count] = 0;
        const mz_bool added = mz_zip_writer_add_mem(&zip, name, contents[index].data,
                (size_t)contents[index].count, (mz_uint)method);
        free(name);
        if (!added) {
            status = writeError(&zip, &output);
            break;
        }
    }
    if (status == COMPRESSION_OK && !mz_zip_writer_finalize_archive(&zip)) {
        status = writeError(&zip, &output);
    }
    if (status == COMPRESSION_OK && mz_zip_is_zip64(&zip)) status = COMPRESSION_LIMIT;
    if (!mz_zip_writer_end(&zip) && status == COMPRESSION_OK) status = nativeError(&zip);
    if (status == COMPRESSION_OK) {
        joyeer_native_bytes_assign(result, output.data, (int64_t)output.count);
    }
    free(output.data);
    return status;
}

static int64_t openReader(mz_zip_archive* zip, const uint8_t* data, int64_t count) {
    if (!validView(data, count, 1)) return COMPRESSION_INVALID_ARGUMENT;
    if (count < 22) return COMPRESSION_INVALID_ARCHIVE;
    if ((uint64_t)count >= UINT32_MAX) return COMPRESSION_UNSUPPORTED;
    if (!mz_zip_reader_init_mem(zip, data, (size_t)count, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY)) {
        return nativeError(zip);
    }
    if (mz_zip_is_zip64(zip)) {
        mz_zip_reader_end(zip);
        return COMPRESSION_UNSUPPORTED;
    }
    return COMPRESSION_OK;
}

static int64_t validateEntryHeader(const uint8_t* data, size_t count,
        const mz_zip_archive_file_stat* stat) {
    // Miniz's callback extractor skips local-header/CRC checks for empty entries
    // and uses only central-directory ZIP64 information.
    const size_t headerSize = 30;
    if (stat->m_local_header_ofs > count || count - (size_t)stat->m_local_header_ofs < headerSize) {
        return COMPRESSION_INVALID_ARCHIVE;
    }
    const uint8_t* header = data + (size_t)stat->m_local_header_ofs;
    if (MZ_READ_LE32(header) != 0x04034b50 ||
        MZ_READ_LE16(header + 6) != stat->m_bit_flag ||
        MZ_READ_LE16(header + 8) != stat->m_method) return COMPRESSION_INVALID_ARCHIVE;
    const mz_uint32 compressed = MZ_READ_LE32(header + 18);
    const mz_uint32 uncompressed = MZ_READ_LE32(header + 22);
    if (compressed == UINT32_MAX || uncompressed == UINT32_MAX) return COMPRESSION_UNSUPPORTED;
    if ((stat->m_bit_flag & 8) == 0 &&
        (MZ_READ_LE32(header + 14) != stat->m_crc32 ||
         compressed != stat->m_comp_size || uncompressed != stat->m_uncomp_size)) {
        return COMPRESSION_INVALID_ARCHIVE;
    }
    const size_t remaining = count - (size_t)stat->m_local_header_ofs - headerSize;
    const size_t nameBytes = MZ_READ_LE16(header + 26);
    size_t extraBytes = MZ_READ_LE16(header + 28);
    if (nameBytes > remaining || extraBytes > remaining - nameBytes ||
        stat->m_comp_size > remaining - nameBytes - extraBytes) return COMPRESSION_INVALID_ARCHIVE;
    const uint8_t* extra = header + headerSize + nameBytes;
    while (extraBytes != 0) {
        if (extraBytes < 4) return COMPRESSION_INVALID_ARCHIVE;
        const size_t fieldBytes = 4 + MZ_READ_LE16(extra + 2);
        if (fieldBytes > extraBytes) return COMPRESSION_INVALID_ARCHIVE;
        if (MZ_READ_LE16(extra) == 1) return COMPRESSION_UNSUPPORTED;
        extra += fieldBytes;
        extraBytes -= fieldBytes;
    }
    return COMPRESSION_OK;
}

int64_t joyeer_compression_list_zip(const uint8_t* data, int64_t count,
        int64_t maximumEntries, int64_t maximumNameBytes, JoyeerArray* result) {
    if (result == NULL || maximumEntries < 0 || maximumNameBytes < 0) return COMPRESSION_INVALID_ARGUMENT;
    mz_zip_archive zip = {0};
    int64_t status = openReader(&zip, data, count);
    if (status != COMPRESSION_OK) return status;
    const mz_uint entries = mz_zip_reader_get_num_files(&zip);
    JoyeerString* names = NULL;
    if ((uint64_t)entries > (uint64_t)maximumEntries || entries > SIZE_MAX / sizeof(*names)) {
        status = COMPRESSION_LIMIT;
    } else if (entries != 0) {
        names = (JoyeerString*)calloc(entries, sizeof(*names));
        if (names == NULL) status = COMPRESSION_NATIVE_ERROR + MZ_ZIP_ALLOC_FAILED;
    }
    int64_t remaining = maximumNameBytes;
    for (mz_uint index = 0; status == COMPRESSION_OK && index < entries; ++index) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, index, &stat)) {
            status = nativeError(&zip);
            break;
        }
        const mz_uint required = mz_zip_reader_get_filename(&zip, index, NULL, 0);
        if (required < 2) {
            status = COMPRESSION_INVALID_ARCHIVE;
            break;
        }
        if ((uint64_t)required - 1 > (uint64_t)remaining) {
            status = COMPRESSION_LIMIT;
            break;
        }
        char* name = (char*)malloc(required);
        if (name == NULL) {
            status = COMPRESSION_NATIVE_ERROR + MZ_ZIP_ALLOC_FAILED;
            break;
        }
        names[index].data = (const uint8_t*)name;
        names[index].count = required - 1;
        if (mz_zip_reader_get_filename(&zip, index, name, required) != required ||
            memchr(name, 0, required - 1) != NULL) {
            status = COMPRESSION_INVALID_ARCHIVE;
            break;
        }
        if ((stat.m_bit_flag & 0x800) == 0) {
            for (mz_uint byte = 0; byte < required - 1; ++byte) {
                if ((uint8_t)name[byte] >= 128) status = COMPRESSION_UNSUPPORTED;
            }
        }
        if (status == COMPRESSION_OK && !joyeer_host_valid_utf8((const uint8_t*)name, required - 1)) {
            status = COMPRESSION_INVALID_ARCHIVE;
        }
        remaining -= required - 1;
    }
    if (!mz_zip_reader_end(&zip) && status == COMPRESSION_OK) status = nativeError(&zip);
    if (status == COMPRESSION_OK) joyeer_native_strings_assign(result, names, entries);
    if (names != NULL) {
        for (mz_uint index = 0; index < entries; ++index) free((void*)names[index].data);
    }
    free(names);
    return status;
}

int64_t joyeer_compression_extract_zip(const uint8_t* data, int64_t count,
        int64_t index, int64_t maximumBytes, JoyeerArray* result) {
    if (result == NULL || index < 0 || maximumBytes < 0) return COMPRESSION_INVALID_ARGUMENT;
    mz_zip_archive zip = {0};
    int64_t status = openReader(&zip, data, count);
    if (status != COMPRESSION_OK) return status;
    OutputBuffer output = {0};
    mz_zip_archive_file_stat stat;
    if ((uint64_t)index >= mz_zip_reader_get_num_files(&zip)) {
        status = COMPRESSION_NOT_FOUND;
    } else if (!mz_zip_reader_file_stat(&zip, (mz_uint)index, &stat)) {
        status = nativeError(&zip);
    } else if (!stat.m_is_supported || stat.m_is_directory || stat.m_is_encrypted ||
            ((stat.m_external_attr >> 16) & 0170000) == 0120000) {
        status = COMPRESSION_UNSUPPORTED;
    } else if (stat.m_uncomp_size > (uint64_t)maximumBytes || stat.m_uncomp_size > SIZE_MAX) {
        status = COMPRESSION_LIMIT;
    } else if ((status = validateEntryHeader(data, (size_t)count, &stat)) == COMPRESSION_OK) {
        output.limit = (size_t)stat.m_uncomp_size;
        if (!mz_zip_reader_extract_to_callback(&zip, (mz_uint)index, writeBuffer, &output, 0)) {
            status = writeError(&zip, &output);
        } else if (output.count != stat.m_uncomp_size ||
                (stat.m_comp_size == 0 && stat.m_crc32 != MZ_CRC32_INIT)) {
            status = COMPRESSION_INVALID_ARCHIVE;
        }
    }
    if (!mz_zip_reader_end(&zip) && status == COMPRESSION_OK) status = nativeError(&zip);
    if (status == COMPRESSION_OK) joyeer_native_bytes_assign(result, output.data, (int64_t)output.count);
    free(output.data);
    return status;
}
