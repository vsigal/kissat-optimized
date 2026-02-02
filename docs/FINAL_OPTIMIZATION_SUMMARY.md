# Final Optimization Summary - F1.CNF

**Date:** 2026-02-02
**Goal:** Improve wall-clock performance on f1.cnf (467K vars, 1.6M clauses)
**Baseline:** 2m52.75s (172.75 seconds)

---

## Optimizations Attempted

### #2: Aggressive Learned Clause Deletion ❌ FAILED
**Expected:** 10-15% speedup
**Actual:** 1-6 seconds **SLOWER**

**Why it failed:**
- F1.cnf doesn't generate clause bloat
- More deletion = more GC overhead
- Default heuristics already optimal

### #1: Inline Binary Storage ✅ ALREADY DONE
**Expected:** 15-25% speedup
**Actual:** Already in Kissat!

### #4: Value Compression ❌ IMPRACTICAL  
**Expected:** 8-12% speedup
**Actual:** Requires 147+ code changes

### #3: Clause Prefetching ❌ MADE IT WORSE
**Expected:** 5-8% speedup
**Actual:** 1 second SLOWER (2m53.9s vs 2m52.7s)

---

## Bottom Line

**Current best:** 2m52.75s with -O3 -march=native -flto

**To go faster you need:**
1. Debug your Phase 1 (split watch lists) - potential 10-15% if fixed
2. Profile-Guided Optimization (PGO) - 3-8%, easy to try
3. Algorithmic improvements - requires weeks/months

**Low-hanging fruit is exhausted.**
