# Rejected Optimization Ideas

This file documents optimization ideas that were attempted but rejected due to:
- No performance improvement
- Performance regression
- Incorrect results
- Excessive complexity for minimal gain

Each entry includes the idea description, implementation details, test results, and reason for rejection.

---

## Batch 1: Ideas 1-5 (First Attempt)
**Date:** 2026-02-06

### Idea #1: Branchless Conflict Analysis
**Problem:** 36% bad speculation in conflict analysis

**Implementation:**
```c
// In deduce.c - analyze_literal()
// Changed from branching to boolean masking
const unsigned should_process = (level > 0) & (!a->analyzed);
if (!should_process) return false;
```

**Results:**
- f1.cnf: Part of broken commit, caused issues
- f2.cnf: Not tested in isolation
- f3.cnf: Not tested in isolation

**Reason for Rejection:** Part of commit that caused 400% slowdown with Idea #2. Not tested individually after issue found.

---

### Idea #2: Aggressive Learned Clause Size Limiting
**Problem:** 11% LLC cache misses from large clauses

**Implementation:**
```c
// In learn.c - kissat_learn_clause()
#define MAX_LEARNED_CLAUSE_SIZE 48
if (size > MAX_LEARNED_CLAUSE_SIZE) {
    // Skip learning large clauses
    kissat_backtrack_after_conflict(solver, jump_level);
    return;
}
```

**Results:**
- f1.cnf: **4+ minutes (400% WORSE)**
- f2.cnf: Not tested
- f3.cnf: Not tested

**Root Cause:** Broken backtracking logic when skipping clauses caused solver to explore many more conflicts unnecessarily.

**Reason for Rejection:** **CRITICAL REGRESSION** - Caused catastrophic performance degradation.

---

### Idea #3: Decision Heap Prefetching
**Problem:** 22% backend bound in heap traversal

**Implementation:**
```c
// In decide.c - largest_score_unassigned_variable()
while (DECIDE_LIKELY (values[LIT (res)])) {
    __builtin_prefetch(&values[LIT(kissat_max_heap(scores))], 0, 0);
    kissat_pop_max_heap (solver, scores);
    res = kissat_max_heap (scores);
}
```

**Results:**
- f1.cnf: 64.39s vs 63.20s (baseline)
- f3.cnf (500k): 46.68s vs 46.78s (baseline)

**Reason for Rejection:** No significant improvement within measurement variance.

---

### Idea #4: Minimization Result Cache
**Problem:** Clause minimization recursively checks same literals

**Implementation:** Skipped - too complex to implement safely

**Reason for Rejection:** High implementation complexity, low expected ROI. Risk of breaking correctness.

---

### Idea #5: Value Array Prefetching in Propagation
**Problem:** Backend bound - random access to values[] array

**Implementation:**
```c
// In proplit.h - PROPAGATE_LITERAL()
// Prefetch values ahead during clause scanning
for (unsigned pf = start_idx + 2; pf < size && pf < start_idx + 6; pf++)
    KISSAT_PROPLIT_PREFETCH(&values[lits[pf]]);

for (; i + 1 < size; i += 2) {
    if (i + 6 < size)
        KISSAT_PROPLIT_PREFETCH(&values[lits[i + 6]]);
    // ... rest of loop
}
```

**Results:**
- f1.cnf: 66.77s vs 63.20s (**5.6% WORSE**)
- f3.cnf (500k): 49.99s vs 46.78s (**6.9% WORSE**)

**Reason for Rejection:** Prefetch overhead exceeded benefits. Cache pollution from over-prefetching.

---

## Batch 2: Ideas 1-5 (Second Attempt)
**Date:** 2026-02-06

### Idea #1: Branch Reordering in Propagation
**Problem:** 36% bad speculation, branch predictor misses

**Implementation:**
```c
// In proplit.h - PROPAGATE_LITERAL()
// Reordered checks: binary first, then satisfied, then conflict
if (LIKELY(head.type.binary)) {
    if (LIKELY(blocking_value > 0)) continue;
    if (blocking_value < 0) { /* conflict */ }
    else { /* assign */ }
}
```

**Results:**
- f2.cnf: 152.02s vs 151.49s (baseline)

**Reason for Rejection:** No improvement within variance. Already well-optimized.

---

### Idea #2: Force Inline Hot Functions
**Problem:** Function call overhead in hot paths

**Implementation:**
```c
// In heap.h and inline.h
static inline __attribute__((always_inline)) bool kissat_empty_heap(...)
static inline __attribute__((always_inline)) unsigned kissat_max_heap(...)
static inline __attribute__((always_inline)) void kissat_push_analyzed(...)
```

**Results:**
- f2.cnf: 150.58s vs 151.49s (~0.6% improvement)
- f3.cnf (500k): 46.47s vs 46.47s (no change)

**Reason for Rejection:** Minimal gain, within measurement variance. Compiler inlining already effective.

---

### Idea #3: Smaller Decision Cache (32 and 16 entries)
**Problem:** 64-entry cache may cause L1 thrashing

**Implementation:**
```c
// In internal.h
#define DECISION_CACHE_SIZE 32  // Was 64
```

**Results:**
- Cache 16: f2.cnf 150.37s, f3.cnf **58.22s (WORSE)**
- Cache 32: f2.cnf 150.13s, f3.cnf 46.41s
- Baseline 64: f2.cnf 150.04s, f3.cnf 46.47s

**Reason for Rejection:** 64 entries already optimal. Smaller caches hurt on some instances.

---

### Idea #4: Fast-Path Clause Minimization
**Problem:** Expensive recursive minimization

**Implementation:** Not attempted

**Reason for Rejection:** Skipped - too complex, risk of breaking correctness for uncertain gains.

---

### Idea #5: Per-Thread Statistics for Mallob
**Problem:** Atomic contention in cluster deployment

**Implementation:** Not attempted

**Reason for Rejection:** Requires Mallob integration testing. Cannot verify standalone.

---

## Summary Statistics

| Batch | Ideas Tested | Accepted | Rejected | Critical Regressions |
|-------|-------------|----------|----------|---------------------|
| 1 | 5 | 0 | 5 | 1 (Idea #2 clause limiting) |
| 2 | 5 | 0 | 5 | 0 |
| **Total** | **10** | **0** | **10** | **1** |

## Key Insights

1. **Current baseline is strong:** f2.cnf ~150s (was ~235s originally = 36% improvement)
2. **Micro-optimizations exhausted:** Branch prediction, prefetching, inlining already optimal
3. **Cache size 64 is optimal:** Extensive testing showed 64 is better than 8, 16, 32, 128
4. **Medium clause optimization (9-16 lits) is the only verified gain** from recent work

## Lessons Learned

1. **Always test f1.cnf first** - catches catastrophic failures quickly
2. **Variance is high** - need multiple runs or longer tests for confirmation
3. **Ideas that work on paper often fail in practice** due to:
   - Unpredictable branch patterns
   - Cache effects
   - Compiler optimizations already doing the work
4. **Safety first** - correctness > performance

---

## How to Add New Entries

When rejecting an idea, add to this file:

```markdown
### Idea #N: Name
**Problem:** What we tried to solve

**Implementation:**
```c
// Code snippet
```

**Results:**
- f1.cnf: Xs vs Ys baseline
- f2.cnf: Xs vs Ys baseline  
- f3.cnf (500k): Xs vs Ys baseline

**Reason for Rejection:** Why it didn't work
```
