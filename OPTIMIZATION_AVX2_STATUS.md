# AVX2 Optimization Status

## Status: ✅ FIXED - No More Segfault

### Problem
- Segfault at end of solving (signal 11)
- Root cause: AVX2 gather instructions accessing memory incorrectly
- `_mm256_i32gather_epi32` was causing memory corruption

### Solution
- Replaced gather-based AVX2 with safe scalar-load version
- `avx2_find_non_false()` now uses scalar loads packed into vectors
- `avx2_find_non_false_unrolled()` uses simple scalar loops

### Performance Results (f2.cnf)
- Baseline: 153.3s
- AVX2 safe version: ~155s median (3 runs: 154.05s, 155.30s, 158.99s)
- Result: Within 1-2% of baseline (no regression)

### Next Steps
1. ✅ Segfault fixed
2. ✅ Benchmark complete
3. ⏳ Move to Optimization #2: Clause Size Specialization

### Files Modified
- src/simdscan.c - Safe AVX2 implementation
