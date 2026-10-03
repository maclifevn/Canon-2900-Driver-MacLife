#define main capt_filter_main
#include "../third_party/captdriver/src/rastertocapt.c"
#undef main

static unsigned char recorded[8192];
static size_t recorded_size;
static void record_band(struct printer_state_s *unused, const void *data, size_t size)
{
    (void)unused;
    if (recorded_size + size > sizeof(recorded)) abort();
    memcpy(recorded + recorded_size, data, size);
    recorded_size += size;
}

static int grouping_test(void)
{
    const size_t sizes[] = {500, 2500, 700, 0};
    struct cached_page_s page = {0};
    struct band_list_s *tail = NULL;
    unsigned char expected[8192];
    size_t total = 0;
    for (unsigned i = 0; i < 4; ++i) {
        struct band_list_s *band = calloc(1, sizeof(*band) + sizes[i]);
        if (!band) abort();
        band->size = sizes[i];
        for (size_t j = 0; j < sizes[i]; ++j)
            expected[total + j] = band->data[j] = (unsigned char)((total + j) % 251);
        total += sizes[i];
        if (tail) tail->next = band; else page.bands = band;
        tail = band;
    }
    const struct printer_ops_s mock_ops = {.send_band = record_band};
    struct printer_state_s mock_state = {.ops = &mock_ops, .printer_blk = 1024};
    send_page_data(&mock_state, &page);
    int failed = total != recorded_size || memcmp(expected, recorded, total);
    while (page.bands) {
        struct band_list_s *next = page.bands->next;
        free(page.bands); page.bands = next;
    }
    if (failed) return 1;
    puts("Band grouping: oversized band preserved without overflow");
    return 0;
}

static int inspect_raster(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;
    cups_raster_t *reader = cupsRasterOpen(fd, CUPS_RASTER_READ);
    if (!reader) return 1;
    struct cups_page_header2_s header;
    unsigned pages = 0;
    while (cupsRasterReadHeader2(reader, &header)) {
        if (!valid_raster_header(&header)) {
            fprintf(stderr, "Native raster rejected: width=%u height=%u bpp=%u bpc=%u color=%u row=%u resolution=%ux%u\n",
                header.cupsWidth, header.cupsHeight, header.cupsBitsPerPixel,
                header.cupsBitsPerColor, header.cupsColorSpace, header.cupsRowCount,
                header.HWResolution[0], header.HWResolution[1]);
            return 1;
        }
        unsigned char *line = malloc(header.cupsBytesPerLine);
        if (!line) abort();
        for (unsigned y = 0; y < header.cupsHeight; ++y)
            if (cupsRasterReadPixels(reader, line, header.cupsBytesPerLine) != header.cupsBytesPerLine)
                return 1;
        free(line);
        ++pages;
    }
    cupsRasterClose(reader); close(fd);
    if (pages != 2) return 1;
    printf("Native PDF rasterization: %u valid A4 monochrome pages\n", pages);
    return 0;
}

int main(int argc, char **argv)
{
    if (grouping_test()) return 1;
    return argc == 2 ? inspect_raster(argv[1]) : 0;
}
