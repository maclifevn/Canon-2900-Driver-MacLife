#include "../third_party/captdriver/src/hiscoa-common.h"
#include "../third_party/captdriver/src/hiscoa-compress.h"
#include "../third_party/captdriver/tests/hiscoa-decompress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng = 0x29002627;
static unsigned random_byte(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng & 255;
}

int main(void)
{
    const unsigned widths[] = {8, 800, 4816, 5104};
    const unsigned rows[] = {1, 69, 70};
    unsigned cases = 0;
    for (unsigned w = 0; w < 4; ++w) {
        for (unsigned h = 0; h < 3; ++h) {
            for (unsigned pattern = 0; pattern < 5; ++pattern) {
                const unsigned line = widths[w] / 8;
                const size_t bytes = line * rows[h];
                unsigned char *input = malloc(bytes);
                unsigned char *compressed = malloc(bytes * 2 + 128);
                unsigned char *decoded = malloc(bytes);
                if (!input || !compressed || !decoded) return 1;
                for (size_t i = 0; i < bytes; ++i) {
                    input[i] = pattern == 0 ? 0 : pattern == 1 ? 255 :
                        pattern == 2 ? (i % 2 ? 0xaa : 0x55) :
                        pattern == 3 ? ((i / line) % 2 ? 0 : 255) : random_byte();
                }
                size_t length = hiscoa_compress_band(compressed, bytes * 2 + 128,
                    input, line, rows[h], HISCOA_EOB_LAST, &hiscoa_default_params);
                const void *cursor = compressed;
                size_t remaining = length, decoded_size = bytes;
                unsigned end_code = hiscoa_decompress_band(&cursor, &remaining, decoded, &decoded_size,
                    line, &hiscoa_default_params);
                /* Upstream always adds 1..32 padding bits after EOB. When
                 * EOB is already word-aligned, the reference decoder leaves
                 * the one all-ones word (XOR 0x43 -> 0xbc) unread. */
                const unsigned char *padding = cursor;
                int valid_padding = remaining == 0 || (remaining == 4 &&
                    padding[0] == 0xbc && padding[1] == 0xbc &&
                    padding[2] == 0xbc && padding[3] == 0xbc);
                if (!length || !valid_padding || end_code != HISCOA_EOB_LAST || decoded_size != bytes || memcmp(input, decoded, bytes)) {
                    fprintf(stderr, "Compression round-trip failed: width=%u rows=%u pattern=%u compressed=%zu remaining=%zu decoded=%zu bytes=%zu equal=%d\n", widths[w], rows[h], pattern, length, remaining, decoded_size, bytes, !memcmp(input, decoded, bytes));
                    return 1;
                }
                ++cases;
                free(input); free(compressed); free(decoded);
            }
        }
    }
    printf("Hi-SCoA round-trip: %u cases passed\n", cases);
    return 0;
}
