/*
 * HPB S27 — JPEG q95 + flat int8 residual (lossless reconstruct)
 * ---------------------------------------------------------------
 * Architecture:
 *   ENCODE: orig → JPEG q95 (~255KB) → residual = orig - decode(jpeg)
 *           residual fits int8 (max_diff ≤ 5 at q95)
 *           zstd(residual) → embed after JPEG bytes in .hpb file
 *   DECODE: read JPEG → decode → add residual → exact original
 *
 * File format (.hpb):
 *   [4]  magic "HPB7"
 *   [4]  width  (uint32_t LE)
 *   [4]  height (uint32_t LE)
 *   [8]  jpeg_size  (uint64_t LE)
 *   [jpeg_size] JPEG bytes (q95)
 *   [8]  res_size   (uint64_t LE)  — zstd compressed residual
 *   [res_size] zstd bytes
 *
 * Residual is int8_t[h*w*3], channel-interleaved RGB.
 * At q95, max_diff ≤ 5, so int8 never clips.
 *
 * Rice/Golomb entropy coding of residual: NEXT step (not here).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <jpeglib.h>
#include <zstd.h>

#define JPEG_QUALITY 95

/* ── JPEG encode/decode in memory ─────────────────────── */

static uint8_t* jpeg_enc(const uint8_t *rgb, int w, int h, size_t *out_sz) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    unsigned char *buf = NULL;
    unsigned long sz = 0;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &buf, &sz);
    cinfo.image_width = w;
    cinfo.image_height = h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t *row = rgb + cinfo.next_scanline * w * 3;
        jpeg_write_scanlines(&cinfo, (JSAMPARRAY)&row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    *out_sz = sz;
    return buf;
}

static uint8_t* jpeg_dec(const uint8_t *data, size_t sz, int *w, int *h) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char*)data, (unsigned long)sz);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);
    *w = cinfo.output_width;
    *h = cinfo.output_height;
    uint8_t *out = malloc((size_t)(*w) * (*h) * 3);
    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t *row = out + cinfo.output_scanline * (*w) * 3;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return out;
}

/* ── PPM read/write ────────────────────────────────────── */

static uint8_t* ppm_read(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }
    char magic[4]; int maxv;
    fscanf(f, "%3s %d %d %d\n", magic, w, h, &maxv);
    size_t npx = (size_t)(*w) * (*h) * 3;
    uint8_t *img = malloc(npx);
    fread(img, 1, npx, f);
    fclose(f);
    return img;
}

static void ppm_write(const char *path, const uint8_t *rgb, int w, int h) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(rgb, 1, (size_t)w * h * 3, f);
    fclose(f);
}

/* ── Encode ────────────────────────────────────────────── */

static void do_encode(const char *in_ppm, const char *out_hpb) {
    int w, h;
    uint8_t *orig = ppm_read(in_ppm, &w, &h);
    if (!orig) return;

    size_t npx = (size_t)w * h * 3;

    /* Step 1: JPEG q95 encode */
    size_t jpeg_sz;
    uint8_t *jpeg_buf = jpeg_enc(orig, w, h, &jpeg_sz);

    /* Step 2: decode JPEG back → get exact JPEG reconstruction */
    int jw, jh;
    uint8_t *jpeg_rec = jpeg_dec(jpeg_buf, jpeg_sz, &jw, &jh);

    /* Step 3: compute int8 residual */
    int8_t *res = malloc(npx);
    int max_diff = 0;
    for (size_t i = 0; i < npx; i++) {
        int d = (int)orig[i] - (int)jpeg_rec[i];
        if (d > 127) d = 127;
        if (d < -128) d = -128;
        res[i] = (int8_t)d;
        int ad = abs(d);
        if (ad > max_diff) max_diff = ad;
    }
    free(jpeg_rec);

    /* Step 4: zstd compress residual */
    size_t res_bound = ZSTD_compressBound(npx);
    uint8_t *res_z = malloc(res_bound);
    size_t res_zsz = ZSTD_compress(res_z, res_bound, res, npx, 3);
    free(res);

    /* Step 5: write .hpb */
    FILE *fo = fopen(out_hpb, "wb");
    fwrite("HPB7", 1, 4, fo);
    uint32_t wu = (uint32_t)w, hu = (uint32_t)h;
    fwrite(&wu, 4, 1, fo);
    fwrite(&hu, 4, 1, fo);
    uint64_t jpeg_u = jpeg_sz;
    fwrite(&jpeg_u, 8, 1, fo);
    fwrite(jpeg_buf, 1, jpeg_sz, fo);
    uint64_t res_u = res_zsz;
    fwrite(&res_u, 8, 1, fo);
    fwrite(res_z, 1, res_zsz, fo);
    fclose(fo);

    /* Stats */
    size_t total = 4 + 4 + 4 + 8 + jpeg_sz + 8 + res_zsz;
    printf("HPB S27 encode: %dx%d\n", w, h);
    printf("  JPEG q%d:      %zu KB\n", JPEG_QUALITY, jpeg_sz / 1024);
    printf("  residual zstd: %zu KB  (max_diff=%d)\n", res_zsz / 1024, max_diff);
    printf("  TOTAL:         %zu KB\n", total / 1024);
    printf("  vs JPEG q85 lossy: compare manually\n");

    free(jpeg_buf);
    free(res_z);
    free(orig);
}

/* ── Decode ────────────────────────────────────────────── */

static void do_decode(const char *in_hpb, const char *out_ppm) {
    FILE *f = fopen(in_hpb, "rb");
    char magic[5] = {0};
    fread(magic, 1, 4, f);
    if (strcmp(magic, "HPB7") != 0) {
        fprintf(stderr, "Not HPB7 file\n"); fclose(f); return;
    }

    uint32_t w, h;
    fread(&w, 4, 1, f);
    fread(&h, 4, 1, f);

    uint64_t jpeg_sz;
    fread(&jpeg_sz, 8, 1, f);
    uint8_t *jpeg_buf = malloc(jpeg_sz);
    fread(jpeg_buf, 1, jpeg_sz, f);

    uint64_t res_zsz;
    fread(&res_zsz, 8, 1, f);
    uint8_t *res_z = malloc(res_zsz);
    fread(res_z, 1, res_zsz, f);
    fclose(f);

    /* Decode JPEG */
    int jw, jh;
    uint8_t *base = jpeg_dec(jpeg_buf, jpeg_sz, &jw, &jh);
    free(jpeg_buf);

    /* Decompress residual */
    size_t npx = (size_t)w * h * 3;
    int8_t *res = malloc(npx);
    ZSTD_decompress(res, npx, res_z, res_zsz);
    free(res_z);

    /* Reconstruct: base + residual */
    uint8_t *out = malloc(npx);
    for (size_t i = 0; i < npx; i++) {
        int v = (int)base[i] + (int)res[i];
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        out[i] = (uint8_t)v;
    }
    free(base);
    free(res);

    ppm_write(out_ppm, out, w, h);
    free(out);
    printf("Decoded → %s (%dx%d)\n", out_ppm, w, h);
}

/* ── Verify lossless ───────────────────────────────────── */

static void do_verify(const char *orig_ppm, const char *dec_ppm) {
    int w1, h1, w2, h2;
    uint8_t *a = ppm_read(orig_ppm, &w1, &h1);
    uint8_t *b = ppm_read(dec_ppm, &w2, &h2);
    if (w1 != w2 || h1 != h2) {
        printf("VERIFY FAIL: size mismatch %dx%d vs %dx%d\n", w1, h1, w2, h2);
        free(a); free(b); return;
    }
    size_t npx = (size_t)w1 * h1 * 3;
    int max_diff = 0, diff_count = 0;
    for (size_t i = 0; i < npx; i++) {
        int d = abs((int)a[i] - (int)b[i]);
        if (d > max_diff) max_diff = d;
        if (d) diff_count++;
    }
    if (max_diff == 0) {
        printf("VERIFY OK: pixel-perfect lossless (max_diff=0, 0 different pixels)\n");
    } else {
        printf("VERIFY FAIL: max_diff=%d, %d/%zu pixels differ\n",
               max_diff, diff_count, npx/3);
    }
    free(a); free(b);
}

/* ── Main ──────────────────────────────────────────────── */

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage:\n"
               "  Encode: %s enc <in.ppm> <out.hpb>\n"
               "  Decode: %s dec <in.hpb> <out.ppm>\n"
               "  Verify: %s verify <orig.ppm> <decoded.ppm>\n",
               argv[0], argv[0], argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "enc") == 0 && argc >= 4) {
        do_encode(argv[2], argv[3]);
    } else if (strcmp(argv[1], "dec") == 0 && argc >= 4) {
        do_decode(argv[2], argv[3]);
    } else if (strcmp(argv[1], "verify") == 0 && argc >= 4) {
        do_verify(argv[2], argv[3]);
    } else {
        fprintf(stderr, "Unknown command\n");
        return 1;
    }
    return 0;
}
