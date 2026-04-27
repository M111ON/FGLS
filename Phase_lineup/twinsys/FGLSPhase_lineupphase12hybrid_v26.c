/*
 * Hybrid Pyramid-Bisect Codec v23 - "GeoSplit + POGLS Packet"
 * ------------------------------------------------------------
 * v22 + three changes:
 *
 * A) Split-Decision Gate (fixes +20KB overhead)
 *    Each tile decides at encode time: NOSPLIT or SPLIT
 *    NOSPLIT → 1 bit (0) + 6-bit Q_flat = 7 bits/tile   (flat region)
 *    SPLIT   → 1 bit (1) + 1-bit diag + 12-bit Q_triAB = 14 bits/tile
 *    Adaptive: flat tiles cost LESS than v21 (was fixed 6 bits)
 *
 * B) POGLS Packet metadata (3-byte tile record)
 *    io_switch[3]   = split flag
 *    io_switch[2]   = diag (only when split=1)
 *    io_switch[1:0] = Q_flat enum (only when split=0)
 *    shift_scale    = dominant variance level (log2 hint for decoder)
 *    data_b/data_a  = reserved (tile spatial address hint)
 *    Pack/unpack via 24-bit POGLS wire format
 *
 * C) GeoSeed 6-Direction Diagonal Picker
 *    v22: gx/gy only (2 axes)
 *    v23: 6 gradient directions sampled from geoseed pattern
 *         dir[0]=E  dir[1]=NE  dir[2]=N  dir[3]=NW  dir[4]=W  dir[5]=SW
 *         Dominant direction → best split axis (diag=0 or diag=1)
 *         Tie-break: anti-diagonal preferred (diagonal edges more common)
 *
 * File: magic HPB6  (incompatible with HPB5)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <zstd.h>
#include <omp.h>

/* ── Config ──────────────────────────────────────────── */
#define TILE          8
#define BASE_TARGET   32
#define MAX_LEVELS     7

/* Split gate: split only when inter-triangle variance gain exceeds this */
#define SPLIT_VAR_GAIN_MIN  0.20f   /* 20% variance reduction needed */
#define SPLIT_VAR_MIN_ABS   4.0f    /* skip split if base var already low */

/* Palette signal map: 12-bit key (R>>4, G>>4, B>>4) → avg variance */
#define PALETTE_SIZE  4096
#define PAL_FLAT_THR  50.0f
#define PAL_EDGE_THR  500.0f
typedef struct { float var_sum; int count; } PalEntry;
static PalEntry g_palette[PALETTE_SIZE];
static float    g_pal_var[PALETTE_SIZE];
static int      g_pal_ready = 0;

static inline int compute_levels(int w, int h) {
    int d = w < h ? w : h;
    int lv = 0;
    while (d > BASE_TARGET && lv < MAX_LEVELS) { d >>= 1; lv++; }
    return lv < 1 ? 1 : lv;
}

/* Adaptive Q thresholds */
#define T_VAR_LOW    6.0f
#define T_VAR_HIGH  18.0f
#define Q_FLAT_BASE 12.0f

/* ── POGLS Packet (3-byte / 24-bit wire format) ───────── */
/*
 * Layout:
 *   [23:16] data_b   = tile_y index (8-bit, truncated)
 *   [15: 8] data_a   = tile_x index (8-bit, truncated)
 *   [ 7: 4] io_switch:
 *             bit7 = split_flag
 *             bit6 = diag (valid when split=1)
 *             bit5 = qA_hi (split=1: qA enum bit1 | split=0: qflat bit1)
 *             bit4 = qA_lo (split=1: qA enum bit0 | split=0: qflat bit0)
 *   [ 3: 1] shift_scale = variance log2 hint (0-7)
 *   [    0] reserved
 */
typedef struct {
    uint8_t data_b;      /* tile_y [0-255] */
    uint8_t data_a;      /* tile_x [0-255] */
    uint8_t io_switch;   /* [split:1][diag:1][q2:2] packed into high nibble */
    uint8_t shift_scale; /* variance level hint [0-7] */
} POGLS_Packet;

static inline uint32_t pogls_pack(POGLS_Packet p) {
    return ((uint32_t)p.data_b      << 16) |
           ((uint32_t)p.data_a      <<  8) |
           ((uint32_t)(p.io_switch  & 0x0F) << 4) |
           ((uint32_t)(p.shift_scale & 0x07) << 1);
}

static inline POGLS_Packet pogls_unpack(uint32_t raw) {
    POGLS_Packet p;
    p.data_b      = (raw >> 16) & 0xFF;
    p.data_a      = (raw >>  8) & 0xFF;
    p.io_switch   = (raw >>  4) & 0x0F;
    p.shift_scale = (raw >>  1) & 0x07;
    return p;
}

/* ── TileMeta (internal, not stored directly) ─────────── */
typedef struct {
    uint8_t split;   /* 0=NOSPLIT, 1=SPLIT */
    uint8_t diag;    /* 0=╲ 1=╱  (valid when split=1) */
    uint8_t qF[3];   /* flat Q enum per channel (split=0) */
    uint8_t qA[3];   /* tri-A Q enum (split=1) */
    uint8_t qB[3];   /* tri-B Q enum (split=1) */
} TileMeta;

/* ── Adaptive bitstream per tile ──────────────────────── */
/* NOSPLIT: [0][qF_R:2][qF_G:2][qF_B:2]         = 7 bits  */
/* SPLIT:   [1][diag:1][qA:6][qB:6]              = 14 bits */
#define NOSPLIT_BITS   7
#define SPLIT_BITS    14

static void tilemeta_write(uint8_t *out, int *bit_pos, const TileMeta *tm) {
    int bit = *bit_pos;
    #define WB(v) do { \
        if (v) out[bit/8] |= (1 << (7 - bit%8)); \
        bit++; \
    } while(0)

    if (!tm->split) {
        WB(0);
        WB((tm->qF[0]>>1)&1); WB(tm->qF[0]&1);
        WB((tm->qF[1]>>1)&1); WB(tm->qF[1]&1);
        WB((tm->qF[2]>>1)&1); WB(tm->qF[2]&1);
    } else {
        WB(1);
        WB(tm->diag & 1);
        for (int c=0; c<3; c++) { WB((tm->qA[c]>>1)&1); WB(tm->qA[c]&1); }
        for (int c=0; c<3; c++) { WB((tm->qB[c]>>1)&1); WB(tm->qB[c]&1); }
    }
    #undef WB
    *bit_pos = bit;
}

static void tilemeta_read(const uint8_t *in, int *bit_pos, TileMeta *tm) {
    int bit = *bit_pos;
    #define RB() ((in[bit/8] >> (7 - bit%8)) & 1); bit++
    tm->split = RB();
    if (!tm->split) {
        for (int c=0; c<3; c++) {
            int hi = RB(); int lo = RB();
            tm->qF[c] = (hi<<1)|lo;
        }
        tm->diag = 0;
        memset(tm->qA, 0, 3); memset(tm->qB, 0, 3);
    } else {
        tm->diag = RB();
        for (int c=0; c<3; c++) { int hi=RB(); int lo=RB(); tm->qA[c]=(hi<<1)|lo; }
        for (int c=0; c<3; c++) { int hi=RB(); int lo=RB(); tm->qB[c]=(hi<<1)|lo; }
        memset(tm->qF, 0, 3);
    }
    #undef RB
    *bit_pos = bit;
}

/* ── Fixed-Point Q (Q8) ───────────────────────────────── */
static inline uint32_t enum_to_qfp(uint8_t e, uint16_t q_fp) {
    if (e == 0) return q_fp;
    if (e == 1) return ((uint32_t)q_fp * 3) >> 1;
    return (uint32_t)q_fp << 1;
}
static inline uint8_t q_to_enum_fp(uint32_t q, uint16_t q_fp) {
    uint32_t lo = ((uint32_t)q_fp * 5) >> 2;
    uint32_t hi = ((uint32_t)q_fp * 7) >> 2;
    if (q <= lo) return 0;
    if (q >= hi) return 2;
    return 1;
}

/* ── GeoSeed 6-Direction Gradient ─────────────────────── */
/*
 *  6 directions from a 36-bit geoseed (6 dirs × 6 bits each)
 *  Sampled offsets (dx,dy) for each direction on the tile:
 *    dir0=E(1,0)  dir1=NE(1,-1) dir2=N(0,-1)
 *    dir3=NW(-1,-1) dir4=W(-1,0) dir5=SW(-1,1)
 *
 *  geoseed built from tile pixel data → deterministic, no state
 */
static const int GEO_DX[6] = { 1,  1, 0, -1, -1, -1};
static const int GEO_DY[6] = { 0, -1,-1, -1,  0,  1};

static uint64_t build_geoseed(const float *pyr_data, int lw, int lh,
                               int tx, int ty) {
    /* Sample 6 pixels at tile corners + center → 6×6 bit descriptor */
    uint64_t seed = 0;
    const int pts[6][2] = {
        {tx,          ty},
        {tx+TILE-1,   ty},
        {tx+TILE/2,   ty+TILE/2},
        {tx,          ty+TILE-1},
        {tx+TILE-1,   ty+TILE-1},
        {tx+TILE/4,   ty+TILE*3/4}
    };
    for (int i=0; i<6; i++) {
        int px = pts[i][0] < lw ? pts[i][0] : lw-1;
        int py = pts[i][1] < lh ? pts[i][1] : lh-1;
        uint8_t v = (uint8_t)pyr_data[(py*lw+px)*3];  /* R channel */
        seed |= ((uint64_t)(v & 0x3F) << (i*6));
    }
    return seed;
}

static uint8_t geoseed_pick_diag(const float *pyr, int lw, int lh,
                                  int tx, int ty) {
    uint64_t geoseed = build_geoseed(pyr, lw, lh, tx, ty);

    /* Extract 6 path_logic values, accumulate gradient per axis */
    int64_t axis_h = 0, axis_v = 0;  /* horizontal vs vertical energy */
    for (int i=0; i<6; i++) {
        uint8_t path_logic = (geoseed >> (i*6)) & 0x3F;
        /* weight each direction by its path_logic magnitude */
        int dx = GEO_DX[i], dy = GEO_DY[i];
        axis_h += (int64_t)path_logic * abs(dx);
        axis_v += (int64_t)path_logic * abs(dy);
    }
    /* Also compute actual gradient for confirmation */
    int32_t gx=0, gy=0;
    for (int y=ty; y<ty+TILE-1 && y<lh-1; y++)
        for (int x=tx; x<tx+TILE-1 && x<lw-1; x++) {
            float p = pyr[(y*lw+x)*3];
            gx += (int32_t)fabsf(pyr[(y*lw+x+1)*3] - p);
            gy += (int32_t)fabsf(pyr[((y+1)*lw+x)*3] - p);
        }

    /* Combine geoseed hint + actual gradient: both must agree for confidence */
    int geo_horiz  = (axis_h > axis_v);
    int grad_horiz = (gx > gy);
    if (geo_horiz == grad_horiz)
        return grad_horiz ? 1 : 0;   /* strong agreement */
    return (gy > gx) ? 1 : 0;        /* fallback: raw gradient */
}

/* Triangle membership */
static inline int tri_id(int lx, int ly, int diag) {
    return diag ? (lx + ly >= TILE ? 1 : 0)
                : (lx > ly         ? 1 : 0);
}

/* ── Types ───────────────────────────────────────────── */
typedef struct { float *data; int h, w; } Layer;
typedef struct { int tile_count; int split_count; float avg_diff; } LevelStats;

static int g_stats_mode = 0;
static LevelStats g_stats[MAX_LEVELS];

static inline float clampf(float v, float l, float h) { return v < l ? l : v > h ? h : v; }

/* ── Bitstream ───────────────────────────────────────── */
typedef struct {
    uint8_t *buf; size_t cap; size_t byte_i; int bit_i;
} BitWriter;

static void bw_init(BitWriter *bw, size_t cap_hint) {
    bw->buf = (uint8_t*)calloc(cap_hint, 1);
    bw->cap = cap_hint; bw->byte_i = 0; bw->bit_i = 7;
}
static void bw_write_bit(BitWriter *bw, int bit) {
    if (bw->byte_i >= bw->cap) {
        bw->cap = bw->cap * 3 / 2 + 64;
        bw->buf = (uint8_t*)realloc(bw->buf, bw->cap);
        memset(bw->buf + bw->byte_i, 0, bw->cap - bw->byte_i);
    }
    if (bit) bw->buf[bw->byte_i] |= (1 << bw->bit_i);
    if (--bw->bit_i < 0) { bw->byte_i++; bw->bit_i = 7; }
}
static size_t bw_finish(BitWriter *bw) {
    return (bw->bit_i == 7) ? bw->byte_i : bw->byte_i + 1;
}
static void bw_free(BitWriter *bw) { free(bw->buf); bw->buf = NULL; }

typedef struct { const uint8_t *buf; size_t byte_i; int bit_i; } BitReader;
static void br_init(BitReader *br, const uint8_t *buf) {
    br->buf = buf; br->byte_i = 0; br->bit_i = 7;
}
static inline int br_read_bit(BitReader *br) {
    int bit = (br->buf[br->byte_i] >> br->bit_i) & 1;
    if (--br->bit_i < 0) { br->byte_i++; br->bit_i = 7; }
    return bit;
}

/* lossless: sign(1)+mag(8), zero=single 0 bit */
static inline void sym2_write(BitWriter *bw, int16_t v) {
    if (v == 0) { bw_write_bit(bw,0); return; }
    bw_write_bit(bw,1);
    bw_write_bit(bw, v<0 ? 1 : 0);
    uint8_t mag = (uint8_t)(v<0 ? -v : v);
    for (int b=7; b>=0; b--) bw_write_bit(bw, (mag>>b)&1);
}
static inline int16_t sym2_read(BitReader *br) {
    if (!br_read_bit(br)) return 0;
    int neg = br_read_bit(br);
    uint8_t mag=0;
    for (int b=7; b>=0; b--) mag |= br_read_bit(br)<<b;
    return neg ? -(int16_t)mag : (int16_t)mag;
}

static float* alloc3(int h, int w) { return (float*)calloc((size_t)h*w*3, sizeof(float)); }
static void free_layer(Layer *l) { if(l->data) free(l->data); l->data=NULL; }

/* nearest-neighbor resize — exact integer, encode/decode identical */
static void resize_sep(const float *src, int sw, int sh, float *dst, int dw, int dh) {
    #pragma omp parallel for schedule(static)
    for (int dy=0; dy<dh; dy++) {
        int sy = (dh>1) ? (int)roundf((float)dy*(sh-1)/(dh-1)) : 0;
        if (sy>=sh) sy=sh-1;
        const float *srow = src+sy*sw*3;
        float *drow = dst+dy*dw*3;
        for (int dx=0; dx<dw; dx++) {
            int sx = (dw>1) ? (int)roundf((float)dx*(sw-1)/(dw-1)) : 0;
            if (sx>=sw) sx=sw-1;
            drow[dx*3+0]=srow[sx*3+0];
            drow[dx*3+1]=srow[sx*3+1];
            drow[dx*3+2]=srow[sx*3+2];
        }
    }
}

/* ── Zlib ─────────────────────────────────────────────── */
static uint8_t* zcompress(const void *src, size_t src_sz, size_t *out_sz) {
    size_t bound=ZSTD_compressBound(src_sz);
    uint8_t *buf=(uint8_t*)malloc(bound);
    *out_sz=ZSTD_compress(buf,bound,src,src_sz,1); /* level 1 = fast encode */
    return buf;
}
static uint8_t* zdecompress(const void *src, size_t src_sz, size_t out_sz) {
    uint8_t *buf=(uint8_t*)malloc(out_sz);
    ZSTD_decompress(buf,out_sz,src,src_sz);
    return buf;
}

/* ── Variance helper (float, used only in decision path) ─ */
static float tile_tri_variance(const float *pyr, const float *pred,
                                int lw, int lh, int tx, int ty, int c,
                                int tri, int diag) {
    double s=0, s2=0; int n=0;
    for (int y=ty; y<ty+TILE&&y<lh; y++)
        for (int x=tx; x<tx+TILE&&x<lw; x++) {
            if (tri >= 0 && tri_id(x-tx,y-ty,diag) != tri) continue;
            double d = pyr[(y*lw+x)*3+c] - pred[(y*lw+x)*3+c];
            s+=d; s2+=d*d; n++;
        }
    if (!n) return 0.0f;
    return (float)((s2 - s*s/n) / n);
}

/* ── Decode ───────────────────────────────────────────── */
void do_decode(const char *in_file, const char *out_file) {
    FILE *f = fopen(in_file, "rb");
    char magic[5]={0}; fread(magic,1,4,f);
    if (strcmp(magic,"HPB8")!=0) { fprintf(stderr,"Not HPB8 file\n"); fclose(f); return; }

    int w,h; fread(&w,4,1,f); fread(&h,4,1,f);
    uint8_t levels_u8; fread(&levels_u8,1,1,f); int levels=levels_u8;
    uint16_t q_fp; fread(&q_fp,2,1,f);

    size_t bsz; fread(&bsz,8,1,f);
    uint8_t *bc=(uint8_t*)malloc(bsz); fread(bc,1,bsz,f);

    int bw=w,bh=h;
    for (int i=0;i<levels;i++){bw/=2;bh/=2;}
    if(bw<1)bw=1; if(bh<1)bh=1;

    uint8_t *bu8=zdecompress(bc,bsz,(size_t)bw*bh*3);
    Layer cur={(alloc3(bh,bw)),bh,bw};
    for(int i=0;i<bw*bh*3;i++) cur.data[i]=bu8[i];
    free(bu8); free(bc);

    for (int li=0; li<levels; li++) {
        int lh,lw; fread(&lh,4,1,f); fread(&lw,4,1,f);
        int n_tiles=((lh+TILE-1)/TILE)*((lw+TILE-1)/TILE);

        /* Read zlib-compressed metadata (v24) */
        size_t meta_bytes, meta_zsz;
        fread(&meta_bytes,8,1,f);
        fread(&meta_zsz,8,1,f);
        uint8_t *meta_zc=(uint8_t*)malloc(meta_zsz); fread(meta_zc,1,meta_zsz,f);
        uint8_t *meta_buf=(uint8_t*)zdecompress(meta_zc,meta_zsz,meta_bytes);
        free(meta_zc);

        TileMeta *tiles = (TileMeta*)malloc((size_t)n_tiles*sizeof(TileMeta));
        int meta_bit = 0;
        for (int i=0; i<n_tiles; i++) tilemeta_read(meta_buf, &meta_bit, &tiles[i]);
        free(meta_buf);

        /* Read residual */
        size_t fsz; fread(&fsz,8,1,f);
        uint8_t *fc=(uint8_t*)malloc(fsz); fread(fc,1,fsz,f);
        int n_vals=lh*lw*3;
        size_t sym_sz=((size_t)n_vals*11+7)/8;
        uint8_t *fb=zdecompress(fc,fsz,sym_sz); free(fc);

        int16_t *flat_res=(int16_t*)malloc((size_t)n_vals*sizeof(int16_t));
        BitReader br; br_init(&br,fb);
        for(int i=0;i<n_vals;i++) flat_res[i]=sym2_read(&br);
        free(fb);

        Layer next={(alloc3(lh,lw)),lh,lw};
        /* round to integer before upsample — match encode pyramid */
        for(int i=0;i<cur.w*cur.h*3;i++) cur.data[i]=roundf(clampf(cur.data[i],0,255));
        resize_sep(cur.data,cur.w,cur.h,next.data,lw,lh);

        int tile_idx=0;
        for(int ty=0;ty<lh;ty+=TILE) for(int tx=0;tx<lw;tx+=TILE) {
            TileMeta *tm=&tiles[tile_idx++];
            for(int y=ty;y<ty+TILE&&y<lh;y++)
                for(int x=tx;x<tx+TILE&&x<lw;x++) {
                    for(int c=0;c<3;c++) {
                        /* lossless: direct delta, no qv scaling */
                        next.data[(y*lw+x)*3+c]+=(float)flat_res[(y*lw+x)*3+c];
                    }
                }
        }
        free_layer(&cur); cur=next;
        free(flat_res); free(tiles);
    }
    fclose(f);

    FILE *fo=fopen(out_file,"wb");
    fprintf(fo,"P6\n%d %d\n255\n",w,h);
    uint8_t *out=(uint8_t*)malloc((size_t)w*h*3);
    for(int i=0;i<w*h*3;i++) out[i]=(uint8_t)clampf(cur.data[i],0,255);
    fwrite(out,1,(size_t)w*h*3,fo); fclose(fo);
    free(out); free_layer(&cur);
    printf("Decoded → %s\n",out_file);
}

/* ── Print Stats ─────────────────────────────────────── */
static void print_stats(int levels) {
    printf("\n══ Benchmark Stats (v26) ════════════════════════════\n");
    printf("%-8s %-8s %-8s %-10s\n","Level","Tiles","Split%","AvgDiff");
    for(int i=0;i<levels;i++){
        LevelStats *s=&g_stats[i];
        float sp=s->tile_count ? 100.0f*s->split_count/s->tile_count : 0;
        printf("L%-7d %-8d %-8.1f %-10.3f\n",i,s->tile_count,sp,s->avg_diff);
    }
    printf("═════════════════════════════════════════════════════\n\n");
}

/* ── Palette Pre-pass ─────────────────────────────────── */
static void palette_build(const float *pyr_data, int lw, int lh) {
    memset(g_palette, 0, sizeof(g_palette));
    for (int ty=0; ty<lh; ty+=TILE) for (int tx=0; tx<lw; tx+=TILE) {
        float sr=0,sg=0,sb=0; int n=0;
        float s2=0, sm=0;
        for (int y=ty; y<ty+TILE&&y<lh; y++)
            for (int x=tx; x<tx+TILE&&x<lw; x++) {
                float r=pyr_data[(y*lw+x)*3+0];
                float g=pyr_data[(y*lw+x)*3+1];
                float b=pyr_data[(y*lw+x)*3+2];
                sr+=r; sg+=g; sb+=b;
                float lum=(r+g+b)/3.0f;
                s2+=lum*lum; sm+=lum; n++;
            }
        if (!n) continue;
        float var = (s2 - sm*sm/n) / n;
        int key = (((int)(sr/n)>>4)<<8)|(((int)(sg/n)>>4)<<4)|((int)(sb/n)>>4);
        key &= (PALETTE_SIZE-1);
        g_palette[key].var_sum += var;
        g_palette[key].count++;
    }
    for (int i=0; i<PALETTE_SIZE; i++)
        g_pal_var[i] = g_palette[i].count ? g_palette[i].var_sum/g_palette[i].count : PAL_EDGE_THR;
    g_pal_ready = 1;
}

static inline float palette_lookup(const float *pyr_data, int lw, int lh, int ty, int tx) {
    float sr=0,sg=0,sb=0; int n=0;
    /* sample 4 corners + center — O(5) instead of O(64) */
    const int pts[5][2]={{0,0},{TILE-1,0},{0,TILE-1},{TILE-1,TILE-1},{TILE/2,TILE/2}};
    for (int i=0; i<5; i++) {
        int x=tx+pts[i][0], y=ty+pts[i][1];
        if (x>=lw) x=lw-1;
        if (y>=lh) y=lh-1;
        sr+=pyr_data[(y*lw+x)*3+0];
        sg+=pyr_data[(y*lw+x)*3+1];
        sb+=pyr_data[(y*lw+x)*3+2];
        n++;
    }
    int key=(((int)(sr/n)>>4)<<8)|(((int)(sg/n)>>4)<<4)|((int)(sb/n)>>4);
    key &= (PALETTE_SIZE-1);
    return g_pal_var[key];
}

/* ── Main Encode ─────────────────────────────────────── */
int main(int argc, char **argv) {
    if(argc<3){
        printf("Usage:\n"
               "  Encode: %s <in.ppm> <out.hpb> [quality] [-stats]\n"
               "  Decode: %s -d <in.hpb> <out.ppm>\n",argv[0],argv[0]);
        return 1;
    }
    if(strcmp(argv[1],"-d")==0){
        if(argc<4){printf("Usage: %s -d <in.hpb> <out.ppm>\n",argv[0]);return 1;}
        do_decode(argv[2],argv[3]); return 0;
    }

    float quality=85.0f;
    for(int i=3;i<argc;i++){
        if(strcmp(argv[i],"-stats")==0) g_stats_mode=1;
        else quality=(float)atof(argv[i]);
    }
    float q_flat=Q_FLAT_BASE-(quality/100.0f)*(Q_FLAT_BASE-4.0f);

    FILE *f=fopen(argv[1],"rb");
    if(!f){fprintf(stderr,"Cannot open %s\n",argv[1]);return 1;}
    int w,h,maxv; char magic_in[4];
    fscanf(f,"%3s %d %d %d\n",magic_in,&w,&h,&maxv);
    uint8_t *img=(uint8_t*)malloc((size_t)w*h*3);
    fread(img,1,(size_t)w*h*3,f); fclose(f);

    int levels=compute_levels(w,h);

    Layer pyr[MAX_LEVELS+1];
    pyr[0]=(Layer){alloc3(h,w),h,w};
    for(int i=0;i<w*h*3;i++) pyr[0].data[i]=img[i];
    free(img);

    for(int i=0;i<levels;i++){
        int nw=pyr[i].w/2, nh=pyr[i].h/2;
        if(nw<1)nw=1; if(nh<1)nh=1;
        pyr[i+1]=(Layer){alloc3(nh,nw),nh,nw};
        resize_sep(pyr[i].data,pyr[i].w,pyr[i].h,pyr[i+1].data,nw,nh);
    }

    uint16_t q_fp=(uint16_t)(q_flat*256.0f+0.5f);

    /* Palette pre-pass on finest level */
    palette_build(pyr[0].data, pyr[0].w, pyr[0].h);

    FILE *fo=fopen(argv[2],"wb");
    fwrite("HPB8",1,4,fo);
    fwrite(&w,4,1,fo); fwrite(&h,4,1,fo);
    uint8_t levels_u8=(uint8_t)levels;
    fwrite(&levels_u8,1,1,fo);
    fwrite(&q_fp,2,1,fo);

    /* Base layer */
    Layer *base=&pyr[levels];
    size_t bsz; uint8_t *bu8=(uint8_t*)malloc((size_t)base->h*base->w*3);
    for(int i=0;i<base->h*base->w*3;i++) bu8[i]=(uint8_t)clampf(base->data[i],0,255);
    uint8_t *bc=zcompress(bu8,(size_t)base->h*base->w*3,&bsz); free(bu8);
    fwrite(&bsz,8,1,fo); fwrite(bc,1,bsz,fo); free(bc);

    /* Encode levels */
    for(int li=0;li<levels;li++){
        int pi=levels-1-li;
        int lh=pyr[pi].h, lw=pyr[pi].w;
        Layer pred_layer={alloc3(lh,lw),lh,lw};
        resize_sep(pyr[pi+1].data,pyr[pi+1].w,pyr[pi+1].h,pred_layer.data,lw,lh);

        int n_tiles_y=(lh+TILE-1)/TILE;
        int n_tiles_x=(lw+TILE-1)/TILE;
        int n_tiles=n_tiles_y*n_tiles_x;

        int16_t  *flat=(int16_t*)calloc((size_t)lh*lw*3,sizeof(int16_t));
        TileMeta *tiles=(TileMeta*)malloc((size_t)n_tiles*sizeof(TileMeta));

        float diff_sum=0; int diff_count=0, split_count=0;

        /* Note: TileMeta computed serially (variance + geoseed, cheap) */
        /* Residual quantize can be parallelized after meta is set       */
        for(int ty=0;ty<lh;ty+=TILE) for(int tx=0;tx<lw;tx+=TILE) {
            int tidx=(ty/TILE)*n_tiles_x+(tx/TILE);
            TileMeta *tm=&tiles[tidx];

            /* ── Split Decision: palette hint first ── */
            float pal_var = palette_lookup(pyr[pi].data, lw, lh, ty, tx);

            if (pal_var < PAL_FLAT_THR) {
                /* Palette says FLAT → skip full variance, go NOSPLIT fast */
                tm->split=0; tm->diag=0;
                for(int c=0;c<3;c++) tm->qF[c]=0;
            } else {
                /* Compute actual variance (needed for Q selection) */
                float var_flat=0;
                for(int c=0;c<3;c++)
                    var_flat+=tile_tri_variance(pyr[pi].data,pred_layer.data,lw,lh,tx,ty,c,-1,0);
                var_flat/=3.0f;

                if(var_flat < SPLIT_VAR_MIN_ABS) {
                    tm->split=0; tm->diag=0;
                    for(int c=0;c<3;c++){
                        int64_t s=0,s2=0; int kc=0;
                        for(int y=ty;y<ty+TILE&&y<lh;y++)
                            for(int x=tx;x<tx+TILE&&x<lw;x++){
                                int32_t d=(int32_t)(pyr[pi].data[(y*lw+x)*3+c]-pred_layer.data[(y*lw+x)*3+c]);
                                s+=d; s2+=d*d; kc++;
                            }
                        if(!kc){tm->qF[c]=0;continue;}
                        int64_t var256=(s2*256-(s*s*256)/kc)/kc;
                        int64_t tlo=(int64_t)T_VAR_LOW*256, thi=(int64_t)T_VAR_HIGH*256;
                        uint32_t qv=(var256<tlo)?q_fp:(var256>thi)?(uint32_t)q_fp<<1:((uint32_t)q_fp*3)>>1;
                        tm->qF[c]=q_to_enum_fp(qv,q_fp);
                    }
                } else {
                    /* ── GeoSeed diagonal pick ── */
                    uint8_t best_diag=geoseed_pick_diag(pyr[pi].data,lw,lh,tx,ty);
                    float var_A=0, var_B=0;
                    for(int c=0;c<3;c++){
                        var_A+=tile_tri_variance(pyr[pi].data,pred_layer.data,lw,lh,tx,ty,c,0,best_diag);
                        var_B+=tile_tri_variance(pyr[pi].data,pred_layer.data,lw,lh,tx,ty,c,1,best_diag);
                    }
                    float var_split=(var_A+var_B)/6.0f;
                    float gain=(var_flat-var_split)/var_flat;

                    if(gain < SPLIT_VAR_GAIN_MIN){
                        tm->split=0; tm->diag=0;
                        for(int c=0;c<3;c++) tm->qF[c]=q_to_enum_fp(q_fp,q_fp);
                    } else {
                        tm->split=1; tm->diag=best_diag; split_count++;
                        for(int t=0;t<2;t++) for(int c=0;c<3;c++) {
                            int64_t s=0,s2=0; int kc=0;
                            for(int y=ty;y<ty+TILE&&y<lh;y++)
                                for(int x=tx;x<tx+TILE&&x<lw;x++){
                                    if(tri_id(x-tx,y-ty,tm->diag)!=t) continue;
                                    int32_t d=(int32_t)(pyr[pi].data[(y*lw+x)*3+c]-pred_layer.data[(y*lw+x)*3+c]);
                                    s+=d; s2+=d*d; kc++;
                                }
                            if(!kc){(t?tm->qB:tm->qA)[c]=0;continue;}
                            int64_t var256=(s2*256-(s*s*256)/kc)/kc;
                            int64_t tlo=(int64_t)T_VAR_LOW*256, thi=(int64_t)T_VAR_HIGH*256;
                            uint32_t qv=(var256<tlo)?q_fp:(var256>thi)?(uint32_t)q_fp<<1:((uint32_t)q_fp*3)>>1;
                            (t?tm->qB:tm->qA)[c]=q_to_enum_fp(qv,q_fp);
                        }
                    }
                }
            }

            /* ── Lossless residuals ── */
            for(int y=ty;y<ty+TILE&&y<lh;y++)
                for(int x=tx;x<tx+TILE&&x<lw;x++) {
                    for(int c=0;c<3;c++){
                        int32_t d=(int32_t)roundf(pyr[pi].data[(y*lw+x)*3+c]-pred_layer.data[(y*lw+x)*3+c]);
                        flat[(y*lw+x)*3+c]=(int16_t)(d<-255?-255:d>255?255:d);
                        if(g_stats_mode){diff_sum+=fabsf((float)d);diff_count++;}
                    }
                }
        }

        if(g_stats_mode){
            g_stats[li].tile_count=n_tiles;
            g_stats[li].split_count=split_count;
            g_stats[li].avg_diff=diff_count?diff_sum/diff_count:0;
        }

        /* Encode flat[] → sym2 → zlib */
        BitWriter bw_s; bw_init(&bw_s,(size_t)lh*lw*3*2+64); /* 11 bits/val worst case */
        int n_vals=lh*lw*3;
        for(int i=0;i<n_vals;i++) sym2_write(&bw_s,flat[i]);
        size_t sym_sz=bw_finish(&bw_s);
        size_t fsz; uint8_t *fc=zcompress(bw_s.buf,sym_sz,&fsz);
        bw_free(&bw_s);

        /* Pack adaptive tilemeta → zlib compressed (v24) */
        size_t meta_bytes=((size_t)n_tiles*SPLIT_BITS+7)/8;
        uint8_t *meta_buf=(uint8_t*)calloc(meta_bytes,1);
        int meta_bit=0;
        for(int i=0;i<n_tiles;i++) tilemeta_write(meta_buf,&meta_bit,&tiles[i]);
        size_t meta_zsz;
        uint8_t *meta_zc=zcompress(meta_buf,meta_bytes,&meta_zsz);
        free(meta_buf);

        fwrite(&lh,4,1,fo); fwrite(&lw,4,1,fo);
        fwrite(&meta_bytes,8,1,fo);   /* uncompressed size for decompress */
        fwrite(&meta_zsz,8,1,fo);
        fwrite(meta_zc,1,meta_zsz,fo);
        fwrite(&fsz,8,1,fo); fwrite(fc,1,fsz,fo);

        free_layer(&pred_layer);
        free(flat); free(tiles); free(meta_zc); free(fc);
    }

    fclose(fo);
    printf("Hybrid v26 Complete. Quality:%.1f q_flat:%.2f LEVELS=%d\n",
           quality,q_flat,levels);
    if(g_stats_mode) print_stats(levels);
    for(int i=0;i<=levels;i++) free_layer(&pyr[i]);
    return 0;
}
