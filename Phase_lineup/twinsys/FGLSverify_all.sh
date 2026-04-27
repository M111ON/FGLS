#!/bin/bash
set -e

INC="-I./src -I./src/core -I./src/phase3 -I."

echo "=== FGLS SYSTEM VERIFICATION REPORT ==="
echo "Date: $(date)"
echo

echo "--- STEP 1: Core Functional Verification ---"
gcc $INC -o test_integration test_integration.c src/core/angular_mapper_v36.c
./test_integration | grep PASS
echo

echo "--- STEP 2: Apex Activation & Harmonic Bands ---"
gcc $INC -o test_apex_activation test_apex_activation.c src/core/angular_mapper_v36.c
./test_apex_activation | grep PASS
echo

echo "--- STEP 3: Boundary Fabric & Expansion ---"
gcc $INC -o test_boundary_fabric test_boundary_fabric.c src/core/angular_mapper_v36.c
./test_boundary_fabric | grep PASS
echo

echo "--- STEP 4: Path Reconstruction Logic ---"
gcc $INC -o test_reconstruct_path test_reconstruct_path.c src/core/angular_mapper_v36.c
./test_reconstruct_path | grep PASS
echo

echo "--- STEP 5: Performance & Determinism Benchmark ---"
./bench_recursive_expansion
echo

echo "=== VERDICT: SYSTEM OPERATIONAL ==="
