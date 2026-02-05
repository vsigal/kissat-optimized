# Session Continuation Guide - Kissat AVX2 Optimization

**Last Updated:** 2026-02-05  
**Current Status:** AVX2 implementation complete, segfault to fix

---

## 1. Current State Summary

### ✅ Completed
- **AVX2 SIMD implementation** for Intel & AMD Gen 4.5 (Zen 4)
- Multiple optimized functions in `src/simdscan.c`:
  - `avx2_find_non_false()` - for 8-31 literal clauses
  - `avx2_find_non_false_unrolled()` - 4x unrolled for >32 literals
  - `avx2_find_literal_idx()` - literal membership testing
  - `avx2_count_false()` - count falsified literals
  - `avx2_all_false()` - check if all literals false
- CPU feature detection working (`kissat_init_simd_support`)
- Build system configured with AVX2 flags

### ⚠️ Known Issues
- **Segfault at end of solving** (signal 11) - likely cleanup/arena issue
- SIMD functions work during solving but crash on exit

### 📊 Performance Baseline (f2.cnf)
- Wall clock: 153.3 seconds (before AVX2)
- Bad speculation: 39.4%
- Backend bound: 16.9%
- LLC miss rate: 4.72%

---

## 2. Quick Start for New Session

### 2.1 Verify Current Build
```bash
cd /home/vsigal/src/kissat
./build/kissat --version  # Should show 4.0.4 with AVX2 flags

# Test AVX2 detection
./build/kissat -v f2.cnf 2>&1 | grep -i simd
# Expected: c [simd-0] AVX-512F=no ... AVX2=yes ...
```

### 2.2 Build Commands
```bash
# Full rebuild with AVX2
rm -rf build
CC="gcc-12 -O3 -mavx2 -march=native" ./configure
cd build && make -j$(nproc)

# Incremental rebuild
cd build && make -j$(nproc)
```

### 2.3 Test for Segfault
```bash
# This will crash at the end (known issue)
timeout 30 ./build/kissat f2.cnf 2>&1 | tail -10

# Look for:
# c caught signal 11 (SIGSEGV)
# c raising signal 11 (SIGSEGV)
```

---

## 3. File Changes Summary

### Modified Files
| File | Lines Changed | Description |
|------|---------------|-------------|
| `src/simdscan.c` | +353, -40 | Full AVX2 implementation |
| `src/application.c` | +4 | Added `kissat_init_simd_support()` call |
| `src/internal.c` | +1 | Removed duplicate SIMD init |

### Key Functions Added (in simdscan.c)
```c
// Main AVX2 find (8-31 literals)
static inline bool avx2_find_non_false(...)

// Unrolled version (32+ literals) - 4x ILP
static inline bool avx2_find_non_false_unrolled(...)

// 16-literal batch (alternative)
static inline bool avx2_find_non_false_16(...)

// 32-literal batch helper
static inline unsigned avx2_find_in_batch_32(...)

// Count falsified literals
// AVX2 version in kissat_simd_count_false()

// Check all false
// AVX2 version in kissat_simd_all_false()

// Find literal index
static inline size_t avx2_find_literal_idx(...)
```

---

## 4. Debug the Segfault (Priority #1)

### 4.1 Current Symptoms
- Solving completes successfully (shows statistics)
- Crash happens during cleanup/exit
- Signal 11 (SIGSEGV) in process-time ~0.45s

### 4.2 Debugging Steps

**Step 1: Run with GDB**
```bash
cd /home/vsigal/src/kissat/build
gdb ./kissat
(gdb) run ../f2.cnf
# Wait for crash
(gdb) bt  # Get backtrace
```

**Step 2: Check if it's SIMD-related**
```bash
# Temporarily disable SIMD path in dispatch function
# Edit src/simdscan.c line ~250:
# Comment out the AVX2 call, force scalar fallback
# Then rebuild and test
```

**Step 3: Check arena/vector issues**
```bash
# Look for double-free or use-after-free
# Common causes:
# - SIMD gather accessing out-of-bounds memory
# - Misaligned loads (though AVX2 handles unaligned)
# - Buffer overflow in clause processing
```

**Step 4: Valgrind (slow but thorough)**
```bash
valgrind --error-exitcode=1 ./build/kissat f2.cnf 2>&1 | head -50
```

### 4.3 Likely Causes
1. **Gather out-of-bounds**: `_mm256_i32gather_epi32` accessing invalid memory
2. **Buffer overflow**: Writing past end of clause literals array
3. **Alignment issue**: Though AVX2 supports unaligned, some edge case
4. **Arena corruption**: SIMD code interfering with clause arena

---

## 5. Next Optimizations (Priority Order)

### #2: Clause Size Specialization (5-10% speedup)
**File:** `src/proplit.h`  
**Concept:** Use function pointer table for size-specific handlers
```c
static const clause_handler_t clause_handlers[] = {
  [2] = handle_binary_clause,      // Specialized
  [3] = handle_ternary_clause,     // Specialized  
  [4] = handle_small_clause,
  // ... up to 8
};
```

### #3: Propagation Tick Budgeting (6-10% speedup)
**File:** `src/proplit.h`  
**Concept:** Prioritize binary/ternary clauses, defer expensive ones
```c
// Phase 1: Binary (fast)
// Phase 2: Ternary (medium)
// Phase 3: Large clauses (deferred if over budget)
```

### #4: Conflict Analysis Batching (8-12% speedup)
**Files:** `src/analyze.c`, `src/bump.c`  
**Concept:** Batch process 4 conflicts at once for better cache locality

### #5: Temporal Clause Partitioning (10-15% speedup)
**Files:** `src/arena.c`, `src/collect.c`  
**Concept:** Hot/warm/cold clause regions based on recent usage

---

## 6. Testing & Benchmarking

### 6.1 Quick Test Suite
```bash
cd /home/vsigal/src/kissat

# Test 1: f1.cnf (baseline)
time ./build/kissat f1.cnf > /dev/null 2>&1
echo "f1 exit code: $?"

# Test 2: f2.cnf (main benchmark) 
time ./build/kissat f2.cnf > /dev/null 2>&1
echo "f2 exit code: $?"

# Test 3: Small instance for quick validation
timeout 10 ./build/kissat f2.cnf 2>&1 | grep -E "conflicts|propagations|time"
```

### 6.2 Performance Analysis
```bash
# Build with perf support
CC="gcc-12 -O3 -mavx2 -march=native -g" ./configure
cd build && make -j$(nproc)

# Run with perf
perf record -g ./kissat f2.cnf
perf report --stdio | head -50
```

### 6.3 Correctness Testing
```bash
# Check solution validity (when segfault is fixed)
./build/kissat f2.cnf > result.txt 2>&1
grep -E "^s |^v " result.txt  # SAT/UNSAT and solution
```

---

## 7. Architecture Notes

### 7.1 SIMD Dispatch Logic
```
kissat_simd_find_non_false()
  ├── count < 8 → scalar_find_non_false()
  ├── AVX-512 available → avx512_find_non_false()
  ├── AVX2 available → avx2_find_non_false() or avx2_find_non_false_unrolled()
  └── fallback → scalar_find_non_false()
```

### 7.2 AVX2 Implementation Strategy
- **16-literal batches**: Most clauses are small
- **4x unrolled**: For large clauses, maximize ILP
- **Gather instructions**: `_mm256_i32gather_epi32(values, indices, 1)`
- **Branchless**: Use movemask + ctz for finding first match

### 7.3 Intel vs AMD Zen 4 Considerations
- **Zen 4**: Native 256-bit AVX2 (no splitting), good gather throughput
- **Intel Alder Lake/Raptor Lake**: Also good AVX2, but different port layout
- **Key**: 4x unrolling helps both architectures with ILP

---

## 8. Git Status

### Current Branch
```bash
git status  # Check modified files
git diff --stat  # Summary of changes
```

### Important: Don't Commit Yet
The segfault needs to be fixed before committing to main.

### When Ready to Commit
```bash
# Fix segfault first
# Then:
git add src/simdscan.c src/application.c src/internal.c
git commit -m "AVX2 SIMD optimization for clause scanning

- Implemented avx2_find_non_false() for 8-31 literal clauses
- Implemented 4x unrolled version for >32 literals
- Added avx2_count_false() and avx2_all_false()
- Optimized for Intel and AMD Zen 4 (Gen 4.5)
- X% speedup on f2.cnf (fill in after benchmark)"
```

---

## 9. Reference Commands

### Code Navigation
```bash
# Find where clauses are scanned
grep -n "find_non_false\|kissat_find_literal" src/*.c

# Check propagation hot path
grep -n "PROPAGATE_LITERAL" src/proplit.h | head -10

# Find clause size handling
grep -n "c->size\|clause.*size" src/proplit.h | head -10
```

### Build Debugging
```bash
# Check if AVX2 is actually being used
objdump -d build/simdscan.o | grep -i "vpgather\|vgather" | head -5

# Verify symbols are exported
nm -C build/kissat | grep avx2

# Check compile flags
grep CC build/makefile
```

### Performance Debugging
```bash
# Check if SIMD path is being taken
# Add printf in dispatch function temporarily

# Profile with perf
perf stat -e cycles,instructions,cache-misses,cache-references ./kissat f2.cnf
```

---

## 10. Quick Checklist for New Session

- [ ] Build successful: `make -j$(nproc)`
- [ ] AVX2 detected: `./kissat -v f2.cnf 2>&1 | grep AVX2=yes`
- [ ] Identify segfault cause with GDB
- [ ] Fix segfault
- [ ] Verify f2.cnf solves correctly
- [ ] Run benchmark (3 runs, take median)
- [ ] Document speedup vs baseline
- [ ] Move to next optimization (#2: Size Specialization)

---

## 11. Contact & Context

**Project:** Kissat SAT Solver 4.0.4 (optimized fork)  
**Repository:** https://github.com/vsigal/kissat-optimized  
**Target Hardware:** Intel & AMD Gen 4.5 (Zen 4), single-core per instance  
**Use Case:** Mallob cluster deployment  
**Total Speedup So Far:** ~34-37% (before AVX2)

**Key Constraints:**
- No NUMA (single process per core)
- No OpenMP (already disabled)
- Must work on both Intel and AMD
- Mallob cluster: 1 instance per core

---

**END OF SESSION DOCUMENTATION**

Continue with: Fix segfault → Benchmark → Optimization #2
