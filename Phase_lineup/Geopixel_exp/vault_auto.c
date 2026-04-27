/*
 * vault_auto.c — Smart Vault Router
 * routes input to best compression tool automatically
 *
 * build: gcc -O2 -o vault_auto vault_auto.c -lz
 *
 * usage:
 *   ./vault_auto enc <file>
 *   ./vault_auto dec <file.vault>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

/* ── Config ── */
#define MAGIC_V2      "VAULT_AUTO_V1"
#define MAX_PATH      2048
#define ZLIB_LEVEL    9

/* ── Route types ── */
typedef enum {
    ROUTE_ZLIB,       /* text/binary/code → zlib in-process    */
    ROUTE_IMAGE,      /* raw image → hybrid_s27 subprocess     */
    ROUTE_VIDEO,      /* video → qhv_codec subprocess          */
    ROUTE_PASSTHROUGH /* already compressed → store as-is      */
} Route;

/* ── Extension table ── */
static const char *EXT_IMAGE[]       = {".ppm",".bmp",".tga",".raw", NULL};
static const char *EXT_VIDEO[]       = {".mp4",".avi",".mkv",".mov",".webm", NULL};
static const char *EXT_PASSTHROUGH[] = {".jpg",".jpeg",".png",".gif",
                                         ".mp3",".aac",".flac",
                                         ".zip",".gz",".zst",".br",
                                         ".7z",".rar", NULL};

static const char *ext_of(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot : "";
}

static int ext_in(const char *ext, const char **table) {
    for (int i = 0; table[i]; i++) {
        if (strcasecmp(ext, table[i]) == 0) return 1;
    }
    return 0;
}

static Route detect_route(const char *path) {
    const char *ext = ext_of(path);
    if (ext_in(ext, EXT_PASSTHROUGH)) return ROUTE_PASSTHROUGH;
    if (ext_in(ext, EXT_IMAGE))       return ROUTE_IMAGE;
    if (ext_in(ext, EXT_VIDEO))       return ROUTE_VIDEO;
    return ROUTE_ZLIB;
}

static const char *route_name(Route r) {
    switch (r) {
        case ROUTE_ZLIB:        return "zlib";
        case ROUTE_IMAGE:       return "hybrid_s27";
        case ROUTE_VIDEO:       return "qhv_codec";
        case ROUTE_PASSTHROUGH: return "passthrough";
    }
    return "unknown";
}

/* ── File helpers ── */
static long file_size(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0) ? (long)st.st_size : -1;
}

static uint8_t *file_read(const char *path, size_t *sz) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *sz = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(*sz);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, *sz, f);
    fclose(f);
    return buf;
}

static int file_write(const char *path, const uint8_t *data, size_t sz) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(data, 1, sz, f);
    fclose(f);
    return 1;
}

/* ── Vault header ──
 * [13]  magic
 * [1]   route  (uint8)
 * [8]   orig_size (uint64 LE)
 * [8]   comp_size (uint64 LE)
 * [32]  reserved
 * [comp_size] data
 */
#define HEADER_SIZE (13 + 1 + 8 + 8 + 32)

static void write_u64le(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) { p[i] = v & 0xFF; v >>= 8; }
}
static uint64_t read_u64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

/* ── ZLIB encode/decode ── */
static uint8_t *zlib_compress_buf(const uint8_t *in, size_t in_sz, size_t *out_sz) {
    uLongf bound = compressBound((uLong)in_sz);
    uint8_t *out = malloc(bound);
    if (!out) return NULL;
    uLongf actual = bound;
    if (compress2(out, &actual, in, (uLong)in_sz, ZLIB_LEVEL) != Z_OK) {
        free(out); return NULL;
    }
    *out_sz = (size_t)actual;
    return out;
}

static uint8_t *zlib_decompress_buf(const uint8_t *in, size_t in_sz,
                                     size_t orig_sz) {
    uint8_t *out = malloc(orig_sz);
    if (!out) return NULL;
    uLongf actual = (uLongf)orig_sz;
    if (uncompress(out, &actual, in, (uLong)in_sz) != Z_OK) {
        free(out); return NULL;
    }
    return out;
}

/* ── Encode: ZLIB path ── */
static int enc_zlib(const char *inpath, const char *outpath) {
    size_t orig_sz;
    uint8_t *data = file_read(inpath, &orig_sz);
    if (!data) { fprintf(stderr, "cannot read %s\n", inpath); return 1; }

    size_t comp_sz;
    uint8_t *comp = zlib_compress_buf(data, orig_sz, &comp_sz);
    free(data);
    if (!comp) { fprintf(stderr, "compress failed\n"); return 1; }

    size_t total = HEADER_SIZE + comp_sz;
    uint8_t *out = calloc(1, total);
    memcpy(out, MAGIC_V2, 13);
    out[13] = (uint8_t)ROUTE_ZLIB;
    write_u64le(out + 14, (uint64_t)orig_sz);
    write_u64le(out + 22, (uint64_t)comp_sz);
    memcpy(out + HEADER_SIZE, comp, comp_sz);
    free(comp);

    file_write(outpath, out, total);
    free(out);

    double ratio = (double)total / (double)orig_sz;
    printf("  zlib: %zu B → %zu B  ratio=%.2fx\n", orig_sz, total, ratio);
    return 0;
}

/* ── Encode: IMAGE path (subprocess hybrid_s27) ── */
static int enc_image(const char *inpath, const char *outpath) {
    char cmd[MAX_PATH * 3];
    /* hybrid_s27 expects .ppm input → .hpb output */
    snprintf(cmd, sizeof(cmd), "./hybrid_s27 enc \"%s\" \"%s\"", inpath, outpath);
    printf("  image → hybrid_s27\n");
    int r = system(cmd);
    if (r != 0) {
        fprintf(stderr, "hybrid_s27 failed (not found?) → fallback zlib\n");
        return enc_zlib(inpath, outpath);
    }
    return 0;
}

/* ── Encode: VIDEO path (subprocess qhv_codec) ── */
static int enc_video(const char *inpath, const char *outpath) {
    char cmd[MAX_PATH * 3];
    snprintf(cmd, sizeof(cmd), "./qhv_codec encode \"%s\" \"%s\"", inpath, outpath);
    printf("  video → qhv_codec\n");
    int r = system(cmd);
    if (r != 0) {
        fprintf(stderr, "qhv_codec failed (not found?) → fallback zlib\n");
        return enc_zlib(inpath, outpath);
    }
    return 0;
}

/* ── Encode: PASSTHROUGH (store as-is in vault header) ── */
static int enc_passthrough(const char *inpath, const char *outpath) {
    size_t orig_sz;
    uint8_t *data = file_read(inpath, &orig_sz);
    if (!data) return 1;

    size_t total = HEADER_SIZE + orig_sz;
    uint8_t *out = calloc(1, total);
    memcpy(out, MAGIC_V2, 13);
    out[13] = (uint8_t)ROUTE_PASSTHROUGH;
    write_u64le(out + 14, (uint64_t)orig_sz);
    write_u64le(out + 22, (uint64_t)orig_sz);
    memcpy(out + HEADER_SIZE, data, orig_sz);
    free(data);

    file_write(outpath, out, total);
    free(out);
    printf("  passthrough: %zu B (already compressed)\n", orig_sz);
    return 0;
}

/* ── Decode ── */
static int do_decode(const char *inpath, const char *outpath) {
    size_t vault_sz;
    uint8_t *vault = file_read(inpath, &vault_sz);
    if (!vault) { fprintf(stderr, "cannot read %s\n", inpath); return 1; }

    if (vault_sz < HEADER_SIZE || memcmp(vault, MAGIC_V2, 13) != 0) {
        fprintf(stderr, "not a vault file\n"); free(vault); return 1;
    }

    Route route    = (Route)vault[13];
    uint64_t orig  = read_u64le(vault + 14);
    uint64_t comp  = read_u64le(vault + 22);
    uint8_t *data  = vault + HEADER_SIZE;

    printf("vault_auto dec: route=%s orig=%llu comp=%llu\n",
           route_name(route), (unsigned long long)orig, (unsigned long long)comp);

    int ret = 0;
    if (route == ROUTE_ZLIB || route == ROUTE_PASSTHROUGH) {
        uint8_t *out = (route == ROUTE_ZLIB)
            ? zlib_decompress_buf(data, (size_t)comp, (size_t)orig)
            : data;  /* passthrough: data is already raw */
        if (!out) { fprintf(stderr, "decompress failed\n"); free(vault); return 1; }
        file_write(outpath, out, (size_t)orig);
        if (route == ROUTE_ZLIB) free(out);
        printf("  decoded → %s\n", outpath);
    } else {
        /* image/video: vault contains subprocess output file — write it out */
        file_write(outpath, data, (size_t)comp);
        printf("  extracted → %s (use hybrid_s27/qhv to final decode)\n", outpath);
    }

    free(vault);
    return ret;
}

/* ── Main ── */
int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage:\n"
               "  %s enc <file>\n"
               "  %s dec <file.vault>\n", argv[0], argv[0]);
        return 1;
    }

    const char *cmd    = argv[1];
    const char *inpath = argv[2];

    if (strcmp(cmd, "enc") == 0) {
        Route r = detect_route(inpath);
        char outpath[MAX_PATH];
        snprintf(outpath, sizeof(outpath), "%s.vault", inpath);

        long orig = file_size(inpath);
        printf("vault_auto enc: %s → route=%s\n", inpath, route_name(r));

        int ret = 0;
        switch (r) {
            case ROUTE_ZLIB:        ret = enc_zlib(inpath, outpath);        break;
            case ROUTE_IMAGE:       ret = enc_image(inpath, outpath);       break;
            case ROUTE_VIDEO:       ret = enc_video(inpath, outpath);       break;
            case ROUTE_PASSTHROUGH: ret = enc_passthrough(inpath, outpath); break;
        }

        if (ret == 0) {
            long out = file_size(outpath);
            printf("  → %s  (%ld B → %ld B  ratio=%.2fx)\n",
                   outpath, orig, out, (double)out / (double)orig);
        }
        return ret;

    } else if (strcmp(cmd, "dec") == 0) {
        char outpath[MAX_PATH];
        /* strip .vault suffix */
        strncpy(outpath, inpath, sizeof(outpath) - 1);
        char *dot = strstr(outpath, ".vault");
        if (dot) *dot = '\0';
        else snprintf(outpath, sizeof(outpath), "%s.out", inpath);
        return do_decode(inpath, outpath);

    } else {
        fprintf(stderr, "unknown command: %s\n", cmd);
        return 1;
    }
}
