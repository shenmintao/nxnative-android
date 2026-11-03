// Complete offline ASTC images, independently compressed. No encoder on Android.
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef struct dm_astc_bank dm_astc_bank;
typedef struct dm_astc_entry {
    uint8_t key[32], sha[32];
    uint32_t flag, format, w, h, levels, layers;
    uint64_t blocks, bytes, source_bytes, offset, length;
} dm_astc_entry;
dm_astc_bank *dm_astc_open(const char *path, char *error, size_t error_size);
void dm_astc_close(dm_astc_bank *bank);
const dm_astc_entry *dm_astc_find(dm_astc_bank *bank, const uint8_t source_sha[32], uint32_t format,
                                uint32_t w, uint32_t h, uint32_t levels, uint32_t layers);
int dm_astc_read(dm_astc_bank *bank, const dm_astc_entry *entry, void *dst, size_t size);
// Geometry is shared by the reader, upload path and host tests; zero is invalid.
uint64_t dm_astc_blocks(uint32_t w, uint32_t h, uint32_t levels, uint32_t layers);
uint32_t dm_astc_source_format(int nvn_format);
