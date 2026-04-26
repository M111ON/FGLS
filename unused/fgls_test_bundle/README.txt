FGLS Standalone Test Bundle
===========================

This bundle contains everything needed to verify the FGLS system on a new Linux environment.

Prerequisites:
- GCC (GNU Compiler Collection)
- Standard C Library (libc)
- Make (optional, but build_and_verify.sh handles it)

How to Run:
1. Open a terminal in this directory.
2. Execute the verification script:
   ./build_and_verify.sh

What it does:
- Compiles and runs Core Integration tests (N1).
- Verifies Apex Activation harmonic bands (M3.1).
- Tests Boundary Fabric expansion (M3.3).
- Validates Path Reconstruction (N2).
- Runs the performance benchmark (100MB simulation).

Structure:
- src/core/   : Fundamental geometric logic.
- src/phase3/ : Integration and high-level pipeline.
- *.c         : Test entry points and benchmarks.
