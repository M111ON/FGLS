/*
 * geo_debug.cu — minimal derive test
 */
#include <stdint.h>
#include <cuda_runtime.h>
#include <stdio.h>

#ifdef _WIN32
#  define FGLS_EXPORT __declspec(dllexport)
#else
#  define FGLS_EXPORT
#endif

__device__ uint64_t _d_mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31; return x;
}

__device__ uint64_t _d_rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

__device__ uint64_t _d_derive(uint64_t core, uint8_t face, uint32_t step) {
    uint64_t salt = ((uint64_t)face << 56) ^ (uint64_t)step;
    uint64_t a    = _d_mix64(core ^ salt);
    uint64_t b    = _d_rotl64(core, (int)((face + step) & 63u));
    return _d_mix64(a ^ b);
}

__global__ void debug_kernel(uint64_t seed, uint64_t *out) {
    /* step 1: simple derive */
    uint64_t r0 = _d_derive(seed, 0u, 0u);
    uint64_t r1 = _d_derive(seed, 1u, 0u);
    uint64_t r2 = _d_derive(r0,   0u, 1u);

    /* step 2: checksum fold */
    uint32_t chk = 0u;
    uint64_t cur = r0;
    for (uint8_t lv = 0; lv < 4u; lv++) {
        chk ^= (uint32_t)(cur ^ (cur >> 32));
        cur  = _d_derive(cur, 0u, (uint32_t)(lv + 1u));
    }

    out[0] = r0;
    out[1] = r1;
    out[2] = r2;
    out[3] = (uint64_t)chk;
}

#ifdef __cplusplus
extern "C"
#endif
FGLS_EXPORT int geo_debug_run(uint64_t seed, uint64_t *results_host) {
    uint64_t *d_out = NULL;
    cudaMalloc(&d_out, 4 * sizeof(uint64_t));
    cudaMemset(d_out, 0, 4 * sizeof(uint64_t));

    debug_kernel<<<1, 1>>>(seed, d_out);
    cudaDeviceSynchronize();
    cudaError_t err = cudaGetLastError();

    cudaMemcpy(results_host, d_out, 4 * sizeof(uint64_t), cudaMemcpyDeviceToHost);
    cudaFree(d_out);
    return (err == cudaSuccess) ? 0 : (int)err;
}
