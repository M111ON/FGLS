/*
 * lc_api.c — C shared library for Python ctypes
 * exposes lcgw_* as C ABI functions
 * build: gcc -O2 -shared -fPIC -o lc_api.so lc_api.c
 */
#include "lc_gcfs_wire.h"
#include <stdlib.h>

static RewindBuffer g_rb;
static LCPalette    g_pal;
static int          g_initialized = 0;

static void ensure_init(void) {
    if (!g_initialized) {
        lcgw_init();
        memset(&g_rb, 0, sizeof(g_rb));
        for (int f = 0; f < 4; f++) g_pal.mask[f] = ~0ull;
        g_initialized = 1;
    }
}

/* ── open: seeds as comma-separated hex string ── */
int api_open(const char *name, uint32_t chunk_count, const char *seeds_hex) {
    ensure_init();
    uint64_t seeds[LCGW_MAX_CHUNKS] = {0};
    const char *p = seeds_hex;
    for (uint32_t i = 0; i < chunk_count && i < LCGW_MAX_CHUNKS; i++) {
        seeds[i] = (uint64_t)strtoull(p, (char **)&p, 16);
        if (*p == ',') p++;
    }
    return lcgw_open(name, chunk_count, seeds, LC_LEVEL_1);
}

/* ── read: returns payload bytes written to out_buf, 0 on block ── */
int api_read(int gfd, uint32_t chunk_idx, uint8_t *out_buf, uint32_t buf_size) {
    ensure_init();
    const uint8_t *p = lcgw_read_payload(gfd, chunk_idx, &g_pal);
    if (!p) return 0;
    uint32_t n = buf_size < GCFS_PAYLOAD_BYTES ? buf_size : GCFS_PAYLOAD_BYTES;
    memcpy(out_buf, p, n);
    return (int)n;
}

/* ── delete: returns chunks_ghosted ── */
int api_delete(int gfd) {
    ensure_init();
    LCDeleteResult r = lcgw_delete(gfd, &g_rb);
    return r.status == 0 ? (int)r.chunks_ghosted : r.status;
}

/* ── rewind: reconstruct chunk from seed into out_buf ── */
int api_rewind(int gfd, uint32_t chunk_idx, uint8_t *out_buf) {
    ensure_init();
    return lcgw_rewind(gfd, chunk_idx, out_buf);
}

/* ── seed: get seed for chunk ── */
uint64_t api_seed(int gfd, uint32_t chunk_idx) {
    ensure_init();
    return lcfs_rewind_seed(lcgw_files[gfd].lc_fd, chunk_idx);
}

/* ── palette: set/clear face bit ── */
void api_palette_set(uint8_t face, uint16_t angle) {
    ensure_init();
    lch_palette_set(&g_pal, face, angle);
}
void api_palette_clear(uint8_t face, uint16_t angle) {
    ensure_init();
    lch_palette_clear(&g_pal, face, angle);
}

/* ── stat: fill json-friendly struct ── */
typedef struct {
    int      lc_fd;
    uint32_t chunk_count;
    uint32_t ghosted;
    uint32_t valid;
} ApiStat;

void api_stat(int gfd, ApiStat *out) {
    ensure_init();
    if (!out) return;
    memset(out, 0, sizeof(ApiStat));
    if (gfd < 0 || gfd >= (int)LCGW_MAX_FILES || !lcgw_files[gfd].open) return;
    LCGWFile *gf = &lcgw_files[gfd];
    out->lc_fd      = gf->lc_fd;
    out->chunk_count = gf->chunk_count;
    for (uint32_t i = 0; i < gf->chunk_count; i++) {
        if (gf->chunks[i].ghosted) out->ghosted++;
        if (gf->chunks[i].valid)   out->valid++;
    }
}
