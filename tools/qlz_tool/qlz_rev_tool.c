/*
 * qlz_rev_tool.c - PC 端压缩工具：生成 type1 反转层（红层）
 * 读取 image_data.h 的点阵，逐字节取反（~），QuickLZ 压缩
 * 输出：compressed_rev.h（C 数组，供固件 type1 发送）
 *
 * 用法: tcc qlz_rev_tool.c quicklz.c -o qlz_rev_tool.exe && qlz_rev_tool.exe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "quicklz.h"

static uint8_t raw_data[4736];
static uint8_t rev_data[4736];

static void read_image_data(const char *path)
{
    FILE *f = fopen(path, "rb");
    char line[512];
    size_t count = 0;

    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }

    while (fgets(line, sizeof(line), f) && count < 4736) {
        char *p = line;
        while (*p && count < 4736) {
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
                unsigned int v;
                if (sscanf(p, "0x%2x", &v) == 1) {
                    raw_data[count++] = (uint8_t)v;
                    p += 4;
                    if (*p == ',') p++;
                } else p++;
            } else p++;
        }
    }
    fclose(f);
    fprintf(stderr, "read %zu bytes from %s\n", count, path);
    if (count != 4736) {
        fprintf(stderr, "WARNING: expected 4736, got %zu\n", count);
    }
}

int main(void)
{
    read_image_data("../src/image_data.h");

    /* 逐字节取反 → 反转层（type1 红层） */
    for (size_t i = 0; i < 4736; i++)
        rev_data[i] = (uint8_t)~raw_data[i];

    size_t cap = qlz_packed_max_compressed_size(4736, QLZ_DEFAULT_BLOCK_SIZE);
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) { fprintf(stderr, "malloc failed\n"); return 1; }

    size_t csize = qlz_packed_compress(rev_data, 4736, out, cap, QLZ_DEFAULT_BLOCK_SIZE);
    if (csize == 0) { fprintf(stderr, "compress failed\n"); return 1; }

    fprintf(stderr, "反转层: 4736 B -> 压缩后: %zu B\n", csize);
    fprintf(stderr, "包数: %zu (每包233B, 最后包=%zuB)\n", (csize + 232) / 233, csize % 233 ? csize % 233 : 233);

    FILE *f = fopen("../src/compressed_rev.h", "w");
    if (!f) { fprintf(stderr, "cannot write output\n"); return 1; }

    fprintf(f, "/* 由 qlz_rev_tool 生成：image_data 逐字节取反 QuickLZ 压缩（type1 反转层/红层） */\n");
    fprintf(f, "/* 原始 4736B -> 压缩 %zuB, 包数 %zu */\n", csize, (csize + 232) / 233);
    fprintf(f, "static const uint8_t compressed_rev[%zu] = {\n", csize);
    for (size_t i = 0; i < csize; i++) {
        if (i % 12 == 0) fprintf(f, "\t");
        fprintf(f, "0x%02X,", out[i]);
        if (i % 12 == 11) fprintf(f, "\n");
    }
    if (csize % 12 != 0) fprintf(f, "\n");
    fprintf(f, "};\n");
    fprintf(f, "#define COMPRESSED_REV_LEN %zu\n", csize);
    fprintf(f, "#define COMPRESSED_REV_PACKETS %zu\n", (csize + 232) / 233);
    fclose(f);

    fprintf(stderr, "已生成 compressed_rev.h (len=%zu, packets=%zu)\n", csize, (csize + 232) / 233);

    free(out);
    return 0;
}
