# Kissat Optimized: Current Status and Next Steps

## Summary

This document summarizes the current state of the Kissat optimization fork, including implemented optimizations, rejected ideas, and the roadmap for future work.

---

## Current Performance Baseline

### Verified Results (gcc-12, -O3 -mavx2 -flto)

| Instance | Time | Status | Notes |
|----------|------|--------|-------|
| f1.cnf | ~63s | SAT | Quick validation test |
| f2.cnf | ~149-151s | SAT | Primary benchmark (was 235s = 37% improvement) |
| f3.cnf (500k conflicts) | ~46-47s | Incomplete | Medium-term test |
| f4.cnf (90s timeout) | ~90s | Incomplete | Long-running profile test |

### Build System

```bash
# Simple build with tests
./build.sh -c

# Manual build
CC="gcc-12 -O3 -mavx2 -march=native -flto" ./configure && make -j
```

---

## Successfully Implemented Optimizations

### 1. AVX2 SIMD Acceleration ✅
- **File:** `src/simdscan.c/h`
- **Impact:** Faster large clause scanning (>16 literals)
- **Status:** Stable, no crashes

### 2. Clause Size Specialization ✅
- **File:** `src/proplit.h`
- **Impact:** Eliminates unpredictable branches
- **Tiers:**
  - Ternary (size==3): Direct indexing
  - Small (4-8): Unrolled 2x scalar
  - Medium (9-16): Simple scalar
  - Large (>16): SIMD

### 3. Decision Cache ✅
- **File:** `src/decide.c`, `src/internal.h`
- **Size:** 64 entries (optimal after testing)
- **Impact:** O(1) average decision time vs O(log n) heap

### 4. Link-Time Optimization (LTO) ✅
- **Build flag:** `-flto`
- **Impact:** Cross-module inlining, smaller binary (~625KB)

### 5. Medium Clause Scalar Path ✅
- **File:** `src/proplit.h`
- **Range:** 9-16 literals
- **Impact:** Avoids SIMD gather latency (~15-20 cycles)

### Combined Results
- **Original baseline:** 235s
- **Current baseline:** ~149-151s
- **Total improvement:** 36-37%

---

## Rejected Optimization Ideas

See `REJECTED_IDEAS.md` for full details. Key rejections:

| Idea | Reason |
|------|--------|
| Aggressive clause size limiting | 400% slowdown (broken backtracking) |
| Value array prefetching | 5-7% worse (cache pollution) |
| Branch reordering | No improvement |
| Force inline hot functions | Within variance |
| Smaller decision cache | 64 entries optimal |

### Key Insight
Micro-optimizations (branch prediction, prefetching, inlining) are exhausted. Current baseline is strong.

---

## Binary Implication Index (Phase 2)

### Infrastructure Created ✅

**Files:**
- `src/binindex.h` - Header with data structures
- `src/binindex.c` - Implementation with incremental add/remove
- Modified `src/internal.h` - Added to solver state

### Initialization & Rebuild Hooks ✅ COMPLETED

**1. Initialization (src/internal.c, src/search.c):**
- `kissat_init_bin_index()` called in `kissat_init()` 
- `kissat_rebuild_bin_index()` called in `start_search()` before search begins
- `kissat_release_bin_index()` called in `kissat_release()` for cleanup

**2. Incremental Rebuild Hooks (src/clause.c):**
- `kissat_bin_impl_add()` called in `new_binary_clause()` when binary clauses are learned
- `kissat_bin_impl_remove()` called in `kissat_delete_binary()` when binary clauses are deleted

**API:**
```c
void kissat_init_bin_index(kissat *solver);      // Initialize empty index
void kissat_release_bin_index(kissat *solver);   // Free memory
void kissat_rebuild_bin_index(kissat *solver);   // Full rebuild from watches
void kissat_bin_impl_add(kissat *solver, unsigned a, unsigned b);     // Add a => b
void kissat_bin_impl_remove(kissat *solver, unsigned a, unsigned b);  // Remove a => b
const bin_impl_entry *kissat_get_bin_impl(kissat *solver, unsigned lit, unsigned *count);
bool kissat_bin_impl_contains(kissat *solver, unsigned lit, unsigned other);
```

**Data Structures:**
```c
typedef struct {
    unsigned *implied;      // Sorted array of implied literals
    unsigned count;         // Number of implications
    unsigned capacity;      // Allocated capacity
} bin_impl_entry;

typedef struct {
    bin_impl_entry *entries;  // One per literal (size = 2*vars)
    unsigned size;            // Number of literals indexed
    bool valid;               // Index is up-to-date
} bin_impl_index;
```

**API:**
```c
void kissat_init_bin_index(kissat *solver);     // Initialize empty
void kissat_build_bin_index(kissat *solver);    // Build from watches
void kissat_release_bin_index(kissat *solver);  // Free memory
unsigned kissat_bin_index_count(kissat *solver, unsigned lit);  // Get count
unsigned kissat_bin_index_get(kissat *solver, unsigned lit, unsigned i);  // Get i-th
bool kissat_bin_index_contains(kissat *solver, unsigned lit, unsigned implied);  // Test membership
```

### Expected Benefits

Based on f4.cnf profile:
- Binary clauses: ~85% of propagation work
- Current approach: Pointer chasing through watch lists
- New approach: O(1) flat array access
- **Expected speedup:** 2-3x for binary clause propagation
- **Overall impact:** 15-30% total solver speedup

### Implementation Status

✅ **Completed:**
- Data structures defined
- Build function from existing watches
- Memory management
- Integration into internal.h

⏳ **Remaining:**
1. **Initialization hooks** - Call `kissat_build_bin_index()` after watch setup
2. **Maintenance hooks** - Rebuild on clause addition/removal
3. **Propagation integration** - Modify `PROPAGATE_LITERAL` to use flat arrays

### Integration Points

```c
// 1. After watch initialization (application.c or similar)
kissat_build_bin_index(solver);

// 2. In propagate.c - PROPAGATE_LITERAL macro
// For binary clauses, use flat array instead of watch traversal:
#ifdef USE_BIN_INDEX
    bin_impl_entry *entry = &solver->bin_impl.entries[lit];
    for (unsigned i = 0; i < entry->count; i++) {
        unsigned implied = entry->implied[i];
        // Process implied literal directly
    }
#else
    // Current watch list traversal
#endif

// 3. Rebuild hooks - in clause addition/removal
kissat_build_bin_index(solver);  // Rebuild from scratch (simpler)
// OR: Incremental update (more complex, potentially faster)
```

---

## Code Architecture

### Key Files

| File | Purpose |
|------|---------|
| `src/proplit.h` | Hot propagation loop - **main optimization target** |
| `src/decide.c` | Decision heuristics - has cache optimization |
| `src/binindex.c/h` | Binary implication index infrastructure |
| `src/internal.h` | Solver state - includes cache and bin_impl |
| `src/analyze.c` | Conflict analysis - 7% of time |
| `src/reduce.c` | Clause reduction - already optimized |
| `src/collect.c` | Garbage collection - triggers compaction |

### Build System

```bash
# Full rebuild
./build.sh -c

# Incremental
make -j -C build

# Clean
./build.sh clean
```

---

## Performance Profiling Summary

### f4.cnf Profile (90s run)

| Component | Time | Notes |
|-----------|------|-------|
| memset | 14.5% | Variable clearing (hard to optimize) |
| Propagation | 13.6% | Binary clauses dominate |
| Clause learning | 12.4% | analyze.c |
| Clause reduction | 6.6% | Already optimized |
| Compaction | 2.8% | Garbage collection |

**Key insight:** Binary clause propagation is the biggest opportunity for further optimization.

---

## Roadmap

### Phase 1: Foundation ✅ COMPLETE
- AVX2 SIMD acceleration
- Clause size specialization
- Decision cache (64 entries)
- LTO
- Medium clause scalar path
- **Result:** 37% improvement (235s → 149s)

### Phase 2: Binary Implication Index ✅ COMPLETE
- ✅ Infrastructure created (binindex.h/c)
- ✅ Initialization hooks (called before search)
- ✅ Incremental maintenance (add/remove hooks on clause changes)
- ✅ Propagation integration (kissat_propagate_binary_index in proplit.h)
- **Expected:** 15-30% additional improvement (pending hard instance testing)
- **Documentation:** BINARY_INDEX_IMPLEMENTATION.md

### Phase 3: Long-Running Optimization ⏳ FUTURE
- f4/f5/f8.cnf specific tuning
- Clause database management for very long runs
- Memory allocation optimization

---

## Testing Protocol

### Quick Validation (f1.cnf)
```bash
timeout 90 build/kissat f1.cnf
# Should complete in ~63s with SAT result
```

### Primary Benchmark (f2.cnf)
```bash
time build/kissat f2.cnf
# Baseline: 149-151s
```

### Medium Test (f3.cnf)
```bash
timeout 100 build/kissat --conflicts=500000 f3.cnf
# Baseline: 46-47s
```

### Profile Run (f4.cnf)
```bash
timeout 90 perf stat build/kissat f4.cnf 2>&1 | tail -30
```

---

## Documentation

| File | Purpose |
|------|---------|
| `IMPLEMENTED_GOOD_IDEAS.md` | Successful optimizations with verification |
| `REJECTED_IDEAS.md` | Failed attempts with analysis |
| `CURRENT_STATUS_AND_NEXT_STEPS.md` | This file - overall status |
| `build.sh` | Build automation with tests |

---

## Quick Reference

### Build
```bash
./build.sh -c
```

### Test
```bash
time build/kissat f2.cnf
```

### Profile
```bash
timeout 90 perf stat build/kissat f4.cnf 2>&1 | tail -30
```

### Clean
```bash
./build.sh clean
```

---

## Conclusion

The optimization fork has achieved significant performance improvements (37% on f2.cnf) through careful, measured optimizations. The current baseline is strong and stable.

The binary implication index infrastructure is ready for integration, which represents the next major opportunity for performance gains (expected 15-30% additional improvement). The remaining work involves wiring the index into the propagation loop and adding maintenance hooks for clause database changes.

All optimization attempts have been carefully documented, including both successes and failures, to guide future work and prevent revisiting dead ends.
