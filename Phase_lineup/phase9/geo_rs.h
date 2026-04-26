/*
 * geo_rs.h — Reed-Solomon over GF(256), systematic Vandermonde
 * ══════════════════════════════════════════════════════════════
 * Layout per block:
 *   k  = FEC_CHUNKS_PER_BLOCK = 12 data symbols (byte-level, per byte position)
 *   n  = fec_n (flexible, 1..12)  parity symbols
 *
 * Encoding:
 *   Systematic: parity[j] = XOR_i( V[j][i] * data[i] )
 *   where V[j][i] = alpha^(i*j), alpha = g^1 = 2
 *   Parity indices j = 1..n  (j=0 is identity row, skipped)
 *
 * Decoding:
 *   Given up to n known-position erasures, solve Vandermonde system.
 *   Uses Gaussian elimination over GF(256) — no Berlekamp-Massey needed
 *   (erasure positions are KNOWN from gap_map).
 *
 * Byte-serial approach:
 *   RS is applied per-byte-position independently across 12 chunks.
 *   Each chunk slot contributes 1 byte at each position.
 *   This keeps code simple (no polynomial object) and is cache-friendly
 *   for sequential byte access.
 *
 * API:
 *   rs_encode(data_bytes[], k, fec_n, parity_out[][])
 *   rs_recover(data_bytes[], k, fec_n, parity_in[][], erasure_pos[], n_erase)
 *   Returns 1 = success, 0 = unrecoverable (n_erase > fec_n)
 *
 * Called by geo_fec.h RS path — never directly by user.
 */
#ifndef GEO_RS_H
#define GEO_RS_H

#include <stdint.h>
#include <string.h>
#include "geo_gf256.h"
#include "geo_tring_stream.h"

/* ── limits ──────────────────────────────────────── */
#define RS_MAX_K    12u   /* data symbols per block  */
#define RS_MAX_N    12u   /* max parity symbols      */

/* ── Vandermonde eval: V[j][i] = alpha^(i*j), alpha=2
 *    j=parity row (1..fec_n), i=data col (0..k-1)    */
static inline uint8_t _rs_vand(uint8_t i, uint8_t j) {
    /* alpha^(i*j) mod 255 */
    if (i == 0u || j == 0u) return 1u;
    uint16_t exp = ((uint16_t)i * j) % 255u;
    return _GF_EXP[exp];
}

/*
 * rs_encode_byte — encode a single byte position across k chunks.
 *   data_col[k]: one byte from each of the k chunks
 *   parity_col[fec_n]: output parity bytes for this position
 */
static inline void rs_encode_byte(
    const uint8_t data_col[RS_MAX_K],
    uint8_t       k,
    uint8_t       fec_n,
    uint8_t       parity_col[RS_MAX_N])
{
    for (uint8_t j = 0u; j < fec_n; j++) {
        uint8_t row = (uint8_t)(j + 1u); /* V row 1..fec_n */
        uint8_t acc = 0u;
        for (uint8_t i = 0u; i < k; i++)
            acc ^= gf256_mul(_rs_vand(i, row), data_col[i]);
        parity_col[j] = acc;
    }
}

/*
 * rs_encode — full RS encode for one block.
 *   data[k][TSTREAM_DATA_BYTES]: k data chunks (byte arrays)
 *   chunk_sz[k]: actual size of each chunk (may differ for last)
 *   fec_n: number of parity chunks to produce
 *   parity_out[fec_n][TSTREAM_DATA_BYTES]: output parity chunks
 *   parity_sz[fec_n]: output — each parity = max(chunk_sz[*])
 */
static inline void rs_encode(
    const uint8_t data[][TSTREAM_DATA_BYTES],
    const uint16_t chunk_sz[RS_MAX_K],
    uint8_t        k,
    uint8_t        fec_n,
    uint8_t        parity_out[][TSTREAM_DATA_BYTES],
    uint16_t       parity_sz[RS_MAX_N])
{
    gf256_init();

    /* determine max chunk size = parity size (zero-pad shorter chunks) */
    uint16_t max_sz = 0u;
    for (uint8_t i = 0u; i < k; i++)
        if (chunk_sz[i] > max_sz) max_sz = chunk_sz[i];

    for (uint8_t j = 0u; j < fec_n; j++) {
        memset(parity_out[j], 0, TSTREAM_DATA_BYTES);
        parity_sz[j] = max_sz;
    }

    uint8_t data_col[RS_MAX_K];
    uint8_t par_col [RS_MAX_N];

    for (uint16_t pos = 0u; pos < max_sz; pos++) {
        /* gather one byte per data chunk (zero if beyond chunk size) */
        for (uint8_t i = 0u; i < k; i++)
            data_col[i] = (pos < chunk_sz[i]) ? data[i][pos] : 0u;

        rs_encode_byte(data_col, k, fec_n, par_col);

        for (uint8_t j = 0u; j < fec_n; j++)
            parity_out[j][pos] = par_col[j];
    }
}

/*
 * rs_recover_byte — solve for missing data bytes at known erasure positions.
 *   known_col[k - n_erase]: bytes from present data chunks
 *   known_idx[k - n_erase]: their chunk indices (0..k-1)
 *   parity_col[fec_n]: parity bytes for this position
 *   erase_idx[n_erase]: missing chunk indices
 *   out_col[n_erase]: recovered bytes, output
 *   Returns 1=ok, 0=singular (shouldn't happen if n_erase <= fec_n)
 *
 * Method: build augmented system A*x = b, Gauss-eliminate over GF(256).
 *   Rows come from parity equations: for each parity j,
 *     sum_i(V[i][j+1]*data[i]) = parity[j]
 *   Substitute known data → move to RHS.
 *   Remaining unknowns = erasure positions.
 *   We need exactly n_erase rows → use first n_erase parity rows.
 */
static inline int rs_recover_byte(
    const uint8_t known_col[RS_MAX_K],
    const uint8_t known_idx[RS_MAX_K],
    uint8_t       n_known,
    const uint8_t parity_col[RS_MAX_N],
    const uint8_t erase_idx [RS_MAX_K],
    uint8_t       n_erase,
    uint8_t       fec_n,
    uint8_t       out_col[RS_MAX_K])
{
    if (n_erase == 0u) return 1;
    if (n_erase > fec_n) return 0; /* beyond capacity */

    /* build n_erase × n_erase system + RHS */
    uint8_t A[RS_MAX_K][RS_MAX_K]; /* augmented [A | b] */
    uint8_t b[RS_MAX_K];

    for (uint8_t r = 0u; r < n_erase; r++) {
        uint8_t row = (uint8_t)(r + 1u); /* parity row index */
        /* RHS: parity[r] XOR sum of known contributions */
        uint8_t rhs = parity_col[r];
        for (uint8_t c = 0u; c < n_known; c++)
            rhs ^= gf256_mul(_rs_vand(known_idx[c], row), known_col[c]);
        b[r] = rhs;
        /* LHS coefficients for erasure columns */
        for (uint8_t c = 0u; c < n_erase; c++)
            A[r][c] = _rs_vand(erase_idx[c], row);
    }

    /* Gaussian elimination with partial pivot over GF(256) */
    for (uint8_t col = 0u; col < n_erase; col++) {
        /* find pivot */
        uint8_t pivot = 255u;
        for (uint8_t r = col; r < n_erase; r++) {
            if (A[r][col]) { pivot = r; break; }
        }
        if (pivot == 255u) return 0; /* singular */

        if (pivot != col) {
            /* swap rows */
            for (uint8_t c = 0u; c < n_erase; c++) {
                uint8_t tmp = A[col][c]; A[col][c] = A[pivot][c]; A[pivot][c] = tmp;
            }
            uint8_t tmp = b[col]; b[col] = b[pivot]; b[pivot] = tmp;
        }

        /* normalize pivot row */
        uint8_t inv = gf256_inv(A[col][col]);
        for (uint8_t c = 0u; c < n_erase; c++)
            A[col][c] = gf256_mul(A[col][c], inv);
        b[col] = gf256_mul(b[col], inv);

        /* eliminate column in all other rows */
        for (uint8_t r = 0u; r < n_erase; r++) {
            if (r == col || !A[r][col]) continue;
            uint8_t factor = A[r][col];
            for (uint8_t c = 0u; c < n_erase; c++)
                A[r][c] ^= gf256_mul(factor, A[col][c]);
            b[r] ^= gf256_mul(factor, b[col]);
        }
    }

    for (uint8_t c = 0u; c < n_erase; c++)
        out_col[c] = b[c];
    return 1;
}

/*
 * rs_recover — full RS recovery for one block.
 *   data[k][TSTREAM_DATA_BYTES]: data array (present chunks have valid data,
 *                                  missing chunks will be written)
 *   present[k]: 1=chunk present, 0=missing
 *   chunk_sz[k]: sizes of present chunks (from parity.chunk_sizes for missing)
 *   parity_in[fec_n][TSTREAM_DATA_BYTES]: parity chunks
 *   parity_sz: parity chunk size (= max data chunk size)
 *   fec_n: number of parity rows available
 *   k: data chunks per block
 *   Returns 1 = fully recovered, 0 = too many losses
 */
static inline int rs_recover(
    uint8_t        data[][TSTREAM_DATA_BYTES],
    const uint8_t  present[RS_MAX_K],
    const uint16_t chunk_sz[RS_MAX_K],    /* known sizes (from FECParity.chunk_sizes) */
    uint8_t        k,
    const uint8_t  parity_in[][TSTREAM_DATA_BYTES],
    uint16_t       parity_sz,
    uint8_t        fec_n)
{
    gf256_init();

    /* collect known/unknown indices */
    uint8_t known_idx[RS_MAX_K], erase_idx[RS_MAX_K];
    uint8_t n_known = 0u, n_erase = 0u;
    for (uint8_t i = 0u; i < k; i++) {
        if (present[i]) known_idx[n_known++] = i;
        else            erase_idx[n_erase++] = i;
    }

    if (n_erase == 0u) return 1;
    if (n_erase > fec_n) return 0; /* unrecoverable */

    /* temp output col buffer */
    uint8_t known_col[RS_MAX_K];
    uint8_t par_col  [RS_MAX_N];
    uint8_t out_col  [RS_MAX_K];

    uint16_t max_sz = parity_sz;

    for (uint16_t pos = 0u; pos < max_sz; pos++) {
        /* gather known data bytes */
        for (uint8_t c = 0u; c < n_known; c++) {
            uint8_t ci = known_idx[c];
            known_col[c] = (pos < chunk_sz[ci]) ? data[ci][pos] : 0u;
        }
        /* gather parity bytes */
        for (uint8_t j = 0u; j < fec_n; j++)
            par_col[j] = parity_in[j][pos];

        int ok = rs_recover_byte(
            known_col, known_idx, n_known,
            par_col, erase_idx, n_erase, fec_n, out_col);
        if (!ok) return 0;

        /* write recovered bytes into data array */
        for (uint8_t c = 0u; c < n_erase; c++) {
            uint8_t ci = erase_idx[c];
            if (pos < chunk_sz[ci]) data[ci][pos] = out_col[c];
            /* positions beyond chunk_sz[ci] stay zero (already zero) */
        }
    }
    return 1;
}

#endif /* GEO_RS_H */
