/*
 * png.c の試験。2 つの PNG を書く:
 *   small.png  8×3、9 色、透過 (tRNS) つき。画素の並びとパレットの番号を見る
 *   big.png    640×400、8 色。stored ブロックが複数 (65535 バイト超) になる経路を通す
 * 中身の検証は tests/run_host_tests.py が Python の zlib で行う
 */
#include <stdio.h>
#include "png.h"

static int sink(void *ctx, const u8 *data, u16 len)
{
    return fwrite(data, 1, len, (FILE *)ctx) != len;
}

static const u8 colors[9 * 3] = {
    0, 0, 0,  0, 0, 255,  255, 0, 0,  255, 0, 255,
    0, 255, 0,  0, 255, 255,  255, 255, 0,  255, 255, 255,
    0, 0, 0,
};

static int write_small(const char *path)
{
    static const u8 alpha[9] = { 255, 255, 255, 255, 255, 255, 255, 255, 0 };
    static const u8 rows[3][4] = {
        { 0x01, 0x23, 0x45, 0x67 },
        { 0x88, 0x88, 0x00, 0x00 },
        { 0x70, 0x70, 0x70, 0x70 },
    };
    struct png_writer w;
    FILE *f = fopen(path, "wb");
    int i, rc;

    if (!f)
        return 1;
    rc = png_begin(&w, sink, f, 8, 3, colors, 9, alpha);
    for (i = 0; i < 3 && !rc; i++)
        rc = png_row(&w, rows[i]);
    if (!rc)
        rc = png_end(&w);
    return fclose(f) != 0 || rc != 0;
}

/* 画素 (x, y) の色は ((x / 80) + (y / 50)) & 7。Python 側が同じ式で期待値を作る */
static int write_big(const char *path)
{
    static u8 row[320];
    struct png_writer w;
    FILE *f = fopen(path, "wb");
    unsigned x, y;
    int rc;

    if (!f)
        return 1;
    rc = png_begin(&w, sink, f, 640, 400, colors, 8, 0);
    for (y = 0; y < 400 && !rc; y++) {
        for (x = 0; x < 320; x++) {
            unsigned left = ((x * 2) / 80 + y / 50) & 7;
            unsigned right = ((x * 2 + 1) / 80 + y / 50) & 7;

            row[x] = (u8)((left << 4) | right);
        }
        rc = png_row(&w, row);
    }
    if (!rc)
        rc = png_end(&w);
    return fclose(f) != 0 || rc != 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: pngtest small.png big.png\n");
        return 2;
    }
    if (write_small(argv[1]) || write_big(argv[2])) {
        printf("FAIL write\n");
        return 1;
    }
    printf("END 0 2\n");
    return 0;
}
