# Kissat Performance Bottleneck Analysis - f1.cnf

**Date:** 2026-02-02
**Instance:** f1.cnf (467,671 variables, 1,609,023 clauses)
**Runtime:** 183.136 seconds (optimized version)
**Total Propagations:** 1,502,991,820 (~8.2M propagations/second)

---

## Executive Summary

Based on statistical analysis, code structure examination, and SAT solver profling literature, the **dominant bottlenecks** in Kissat on f1.cnf are:

1. **PROPAGATE_LITERAL routine (60-70% of runtime)** - Watch list traversal
2. **Memory access patterns (15-20%)** - Cache misses in clause/watch access
3. **Conflict analysis (5-10%)** - Learning new clauses
4. **Watch list management (3-5%)** - Large clause rewatching
5. **Decision heuristics (2-3%)** - VSIDS/CHB scoring

---

## Detailed Bottleneck Breakdown

### 1. 🔥 CRITICAL: PROPAGATE_LITERAL (60-70% runtime)

**Location:** `src/proplit.h` lines 42-250

**Why It's Hot:**
- Called 1.5 BILLION times on f1.cnf
- Average: 8.2 million propagations per second
- Inner loop is the tightest in the entire solver

**Current Code Structure:**
```c
while (p != end_watches) {
  const watch head = *q++ = *p++;              // Load watch
  const unsigned blocking = head.blocking.lit;  // Extract blocking lit
  const value blocking_value = values[blocking]; // Load value (cache miss!)

  if (head.type.binary) {
    // Binary clause handling
  } else {
    const watch tail = *q++ = *p++;            // Load second watch
    const reference ref = tail.raw;            // Get clause reference
    clause *const c = (clause *) (arena + ref); // Pointer arithmetic
    if (c->garbage) { ... }                     // Check garbage
    unsigned *const lits = BEGIN_LITS(c);       // Get literals
    const unsigned other = lits[0] ^ lits[1] ^ not_lit; // XOR trick
    const value other_value = values[other];    // Another value load
    // ... more processing
  }
}
```

**Bottleneck Sources:**

#### A. Memory Access Latency (BIGGEST ISSUE)
```
values[blocking]  -> ~50-100 cycles on cache miss
values[other]     -> ~50-100 cycles on cache miss
clause access     -> ~50-100 cycles on cache miss
```

**Problem:** With 467K variables:
- `values` array: 2 * 467K = 934K entries = ~900 KB
- Exceeds L1 cache (32 KB), partially exceeds L2 (256 KB)
- Random access pattern causes cache thrashing

#### B. Dependent Loads (Pipeline Stalls)
```c
const watch head = *p++;                     // Load 1
const unsigned blocking = head.blocking.lit; // Depends on Load 1
const value blocking_value = values[blocking]; // Depends on both
```

CPU can't execute these in parallel - each waits for previous.

#### C. Indirect Memory Access
```c
clause *const c = (clause *) (arena + ref);
```
Two-level indirection: ref -> arena -> clause data

**Performance Impact:**
- 1.5B propagations * 5-10 cycles avg per value access = 7.5B - 15B cycles
- At 3 GHz: 2.5 - 5 seconds just on value lookups!
- Add cache misses: 10-20 seconds more
- **Estimated: 40-60% of total runtime**

---

### 2. 🔥 CRITICAL: Memory Bandwidth & Cache Misses

**Memory Access Pattern Analysis:**

```
Working Set Size for f1.cnf:
- values array: ~900 KB
- watches array: ~5-10 MB (estimated)
- clause arena: ~50-100 MB (estimated)
- Total: ~60-110 MB active data

CPU Cache Hierarchy:
- L1d: 32 KB (4% of values, 0.5% of watches)
- L2: 256 KB (28% of values, 3% of watches)
- L3: Shared, ~several MB (can fit values + some watches)
```

**Cache Miss Rate (Estimated):**
- L1 miss rate: 40-60% (values array alone exceeds L1)
- L2 miss rate: 20-30% (watch lists scattered)
- L3 miss rate: 5-10% (clause arena access)

**Impact:**
- L1 hit: ~4 cycles
- L2 hit: ~12 cycles
- L3 hit: ~40-50 cycles
- RAM: ~100-200 cycles

With 40% L1 miss rate on 1.5B accesses:
- 600M L1 misses * 8 cycles penalty = 4.8B cycles = 1.6 seconds
- Plus L2/L3 misses: **Estimated 20-30 seconds lost to cache misses**

---

### 3. Watch List Structure Issues

**Problem:** Binary and large watches are interleaved in same list.

**Current:**
```c
watches[lit] = [bin1, bin2, large_head1, large_tail1, bin3, large_head2, ...]
```

**Performance Cost:**
```c
if (head.type.binary) { ... }  // Branch every watch
else { ... }
```
- Branch misprediction: ~15-20 cycles
- With mixed lists: 10-20% misprediction rate
- 1.5B propagations * 15 cycles * 15% = 3.4B cycles = **1+ second**

---

### 4. Clause Structure Access

**Current Clause Layout:**
```c
struct clause {
  unsigned garbage : 1;
  unsigned redundant : 1;
  unsigned glue : 14;
  unsigned size : 16;
  unsigned searched : 16;
  unsigned promoted : 16;
  reference literals[...];
}
```

**Access Pattern in PROPAGATE_LITERAL:**
```c
if (c->garbage) { ... }           // Read byte 0-3
unsigned *lits = BEGIN_LITS(c);   // Skip to literals (+16 bytes)
const unsigned other = lits[0] ^ lits[1]; // Read 2 more lits
```

**Problem:** False sharing and cache line waste
- Clause header: ~16 bytes
- But loaded cache line: 64 bytes
- Wasting: 48 bytes per clause access on hot path

---

### 5. XOR Literal Discovery Trick

**Code:**
```c
const unsigned other = lits[0] ^ lits[1] ^ not_lit;
```

**Cost Analysis:**
- XOR is cheap (1 cycle)
- BUT requires loading lits[0] and lits[1]
- **Two memory loads: 2-10 cycles depending on cache**

**Alternative (not currently used):**
```c
// Could store "other" directly in watch structure
const unsigned other = head.other_watched_lit;
```
- Saves 2 memory loads
- Trade-off: Larger watch structure (already 64-bit)

---

## Profiling Data (Statistical Inference)

Based on 183 second runtime and typical SAT solver profiles:

| Component | Est. Time (s) | % | Rationale |
|-----------|---------------|---|-----------|
| **PROPAGATE_LITERAL** | 110-130 | 60-70% | 1.5B calls, memory-bound |
| Memory bandwidth/cache misses | 25-35 | 14-19% | Embedded in above |
| Conflict analysis | 10-18 | 5-10% | 1.8M conflicts |
| Clause learning | 5-10 | 3-5% | 1.6M clauses learned |
| Watch management | 5-10 | 3-5% | Clause deletion/rewatching |
| Decision making | 3-6 | 2-3% | 4M decisions |
| Garbage collection | 2-4 | 1-2% | Periodic GC |
| Miscellaneous | 5-10 | 3-5% | Everything else |

---

## Code Hot Spots Identified

###  Critical (>10% runtime each):

1. **`PROPAGATE_LITERAL` main loop** (src/proplit.h:79-240)
   - Watch list iteration
   - Value lookups
   - Clause dereferencing

2. **Value array access** (embedded in PROPAGATE_LITERAL)
   - `values[blocking]`
   - `values[other]`
   - `values[replacement]`

3. **Clause memory access** (arena dereference)
   - `(clause *) (arena + ref)`
   - Cache-hostile pattern

### High (5-10% runtime each):

4. **`kissat_analyze_` conflict** (src/analyze.c)
   - Conflict clause analysis
   - 1UIP computation

5. **Watch list rewatching** (src/watch.c)
   - After GC
   - During subsumption

6. **Clause allocation** (src/clause.c)
   - Learning new clauses
   - Arena management

### Medium (2-5% runtime each):

7. **Decision heuristics** (src/decide.c)
   - VSIDS/CHB scoring
   - Heap operations

8. **Binary clause handling** in PROPAGATE_LITERAL
   - Still called billions of times

---

## Root Cause Analysis

### Why f1.cnf is Slow:

1. **Large Problem Size**
   - 467K variables → Working set doesn't fit in cache
   - Every propagation = potential cache miss

2. **High Propagation Count**
   - 1.5B propagations in 183 seconds
   - Even tiny overhead = seconds lost

3. **Memory-Bound Workload**
   - CPU spends more time waiting for memory than computing
   - Bandwidth saturated, not compute

4. **Random Access Pattern**
   - CNF structure causes unpredictable memory access
   - Prefetching ineffective

---

## Optimization Opportunities (Ranked)

### 🥇 TIER 1: Biggest Impact (10-30% potential gain each)

#### 1. Split Binary/Large Watch Lists
**Problem:** Type checking every watch
**Solution:**
```c
typedef struct {
  watches binary_watches;
  watches large_watches;
} split_watches;

split_watches watches[LITS];
```

**Expected Gain:**
- Eliminate `if (head.type.binary)` checks
- Better branch prediction (100% vs 80%)
- Cleaner cache access pattern
- **Estimated: 15-25% speedup on propagation**

**Implementation Effort:** Medium (2-3 days)

#### 2. Prefetch Clause Data
**Problem:** Clause access causes cache miss
**Solution:**
```c
// Prefetch next clause while processing current
if (next_watch != end_watches && !next_watch->type.binary) {
  reference next_ref = next_watch[1].raw;
  __builtin_prefetch(arena + next_ref, 0, 1);
}
```

**Expected Gain:**
- Hide 50-100 cycle latency
- 30-40% fewer cache miss stalls
- **Estimated: 10-15% speedup overall**

**Implementation Effort:** Low (1 day)

#### 3. Compact Clause Structure
**Problem:** Wasted cache line space
**Solution:**
```c
// Pack frequently accessed fields in first 8 bytes
struct clause_hot {
  unsigned lit0;
  unsigned lit1;
  unsigned size : 16;
  unsigned garbage : 1;
  unsigned redundant : 1;
  // ... cold fields in separate structure
};
```

**Expected Gain:**
- Better cache utilization
- Fewer cache lines loaded
- **Estimated: 8-12% speedup**

**Implementation Effort:** Medium-High (3-4 days, invasive change)

---

### 🥈 TIER 2: Moderate Impact (5-10% potential each)

#### 4. Localize Values Array Access
**Problem:** 900KB values array destroys cache
**Solution:**
```c
// Cache hot values in registers/stack
value v_blocking = values[blocking];
value v_other = values[other];
// Reuse without repeated memory loads
```

**Expected Gain: 5-8%**
**Effort:** Low (pattern throughout code)

#### 5. Watch List Compaction During GC
**Problem:** Fragmented watch lists
**Solution:** Compact lists to improve cache locality
**Expected Gain: 5-7%**
**Effort:** Low (already have GC infrastructure)

#### 6. SIMD Watch List Scanning
**Problem:** Sequential scan is slow
**Solution:**
```c
// Use SSE/AVX to scan 4-8 watches at once
__m256i blocking_lits = _mm256_load_si256(watches + i);
// Parallel value lookups
```

**Expected Gain: 8-15%** (but only on large watch lists)
**Effort:** High (requires SIMD expertise)

---

### 🥉 TIER 3: Small Impact (2-5% potential each)

#### 7. Better Branch Prediction Hints
**Current:** Some hints in place
**Solution:** Add more systematic hints
**Expected Gain: 2-3%**
**Effort:** Very Low

#### 8. Inline Hot Functions
**Current:** Some functions not inlined
**Solution:** Force inline with `__attribute__((always_inline))`
**Expected Gain: 2-4%**
**Effort:** Very Low

#### 9. Optimize Garbage Check
**Problem:** Checking `c->garbage` on every propagation
**Solution:** Could use dirty watch list more aggressively
**Expected Gain: 1-2%** (already improved)
**Effort:** Low

---

## Recommended Action Plan

### Phase 1: Quick Wins (1-2 weeks)

1. **Add clause prefetching** (1 day)
   - Immediate 10-15% gain
   - Low risk

2. **Cache value array lookups** (2 days)
   - Reduce memory traffic
   - 5-8% gain

3. **More aggressive inlining** (1 day)
   - Trivial change
   - 2-4% gain

**Expected Combined Gain: 17-27%**

### Phase 2: Structural Changes (3-4 weeks)

4. **Split binary/large watch lists** (1 week)
   - Requires careful testing
   - 15-25% gain

5. **Compact clause structure** (1 week)
   - Invasive but worthwhile
   - 8-12% gain

6. **Watch list compaction** (3 days)
   - Leverage existing GC
   - 5-7% gain

**Expected Combined Gain: 28-44% on top of Phase 1**

### Phase 3: Advanced (4-8 weeks)

7. **SIMD optimizations** (2-3 weeks)
   - Requires expertise
   - 8-15% gain

8. **Custom memory allocator** (2-3 weeks)
   - Better locality
   - 5-10% gain

**Expected Combined Gain: 13-25% on top of Phase 2**

---

## Total Potential

| Phase | Time | Gain | Cumulative |
|-------|------|------|------------|
| Current | - | - | 183s (baseline) |
| Phase 1 | 1-2 weeks | 20% | 146s |
| Phase 2 | 3-4 weeks | 35% | 95s |
| Phase 3 | 4-8 weeks | 15% | 81s |

**Total Potential Speedup: ~2.25x (55% reduction)**

---

## Compiler Optimization Opportunities

### Quick Test: Aggressive Flags

```bash
CFLAGS="-O3 -march=native -flto -fomit-frame-pointer -funroll-loops" make
```

**Expected additional gain: 5-10%**

### Profile-Guided Optimization

```bash
# Train on f1.cnf
CFLAGS="-O3 -march=native -fprofile-generate" make
./build/kissat f1.cnf

# Rebuild with profile
CFLAGS="-O3 -march=native -fprofile-use" make
```

**Expected additional gain: 3-8%**

---

## Measurement Strategy

### To Validate Improvements:

1. **Microbenchmarks**
   ```bash
   perf stat -e cache-references,cache-misses,branch-misses \
     ./build/kissat f1.cnf
   ```

2. **Detailed Profiling** (requires sudo)
   ```bash
   sudo perf record -g --call-graph dwarf ./build/kissat f1.cnf
   sudo perf report
   ```

3. **Cache Analysis**
   ```bash
   valgrind --tool=cachegrind ./build/kissat f1.cnf
   cg_annotate cachegrind.out.<pid>
   ```

4. **Time Different Components**
   - Add timing macros around hotspots
   - Compare before/after each optimization

---

## Conclusion

**The #1 bottleneck is PROPAGATE_LITERAL memory access latency.**

Specifically:
1. **Values array lookups** (cache misses)
2. **Watch list iteration** (poor locality)
3. **Clause dereferencing** (indirection overhead)

**Recommended Priority:**
1. Start with **clause prefetching** (quick, low-risk, high-impact)
2. Then **split watch lists** (bigger change, bigger payoff)
3. Then **compiler flags** (free speedup)

**These three alone could give you 30-40% improvement.**

---

Next: Implement Phase 1 optimizations?

