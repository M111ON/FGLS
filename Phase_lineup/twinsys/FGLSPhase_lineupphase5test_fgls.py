import ctypes, time

class ApexSeed(ctypes.Structure):
    _pack_ = 1
    _fields_ = [("seed", ctypes.c_uint64), ("dispatch_id", ctypes.c_uint32)]

class ApexResult(ctypes.Structure):
    _fields_ = [("coset_checksum", ctypes.c_uint32 * 12),
                ("master_fold", ctypes.c_uint32),
                ("verify_ok", ctypes.c_uint32),
                ("dispatch_id", ctypes.c_uint32),
                ("_pad", ctypes.c_uint32)]

lib = ctypes.CDLL('./geo_seed_v2.so')
lib.geo_seed_pipeline.argtypes = [
    ctypes.POINTER(ApexSeed),
    ctypes.POINTER(ApexResult),
    ctypes.c_uint32
]
lib.geo_seed_pipeline.restype = ctypes.c_int

BATCH = 65536
h_seeds   = (ApexSeed   * BATCH)()
h_results = (ApexResult * BATCH)()

for i in range(BATCH):
    h_seeds[i].seed = 0xABCDEF1234567890 + i
    h_seeds[i].dispatch_id = i

# warmup
lib.geo_seed_pipeline(h_seeds, h_results, BATCH)

# bench
start = time.perf_counter()
for _ in range(100):
    lib.geo_seed_pipeline(h_seeds, h_results, BATCH)
end = time.perf_counter()

tput = (BATCH * 100) / (end - start)
print(f"Throughput:  {tput:,.0f} cubes/sec")
print(f"verify_ok:   {h_results[0].verify_ok} (expect 1)")
print(f"master_fold: {h_results[0].master_fold:#010x}")
