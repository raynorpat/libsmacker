#ifndef SMK_PALETTE_H
#define SMK_PALETTE_H

#include <stddef.h>
#include <stdint.h>

extern const uint8_t smk_palmap[64];

int smk_palette_apply(uint8_t palette[768], const uint8_t *chunk, size_t size);
size_t smk_palette_write_chunk(const uint8_t *entries, unsigned count,
                               uint8_t *out);

#endif
