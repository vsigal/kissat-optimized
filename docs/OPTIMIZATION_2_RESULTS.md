# Optimization #2: Aggressive Learned Clause Deletion - Results

**Date:** 2026-02-02
**Target:** f1.cnf (467K variables, 1.6M clauses)
**Goal:** Reduce learned clause bloat to speed up propagation

---

## What Was Implemented

### Changes Made

1. **Aggressive deletion fraction for large instances** (src/reduce.c):
   - Detect instances with >400K variables
   - Keep only 15-20% of learned clauses (vs default 50-90%)
   - More aggressive tier limits (glue ≤ 2-3 protected vs default dynamic)

2. **More frequent reductions** (src/kimits.c):
   - Tried: Initial reduction after 500 conflicts (vs default 1000)
   - Tried: Fixed 300-conflict intervals
   - **Final:** Reverted to default frequency

3. **Stricter glue-based protection** (src/reduce.c):
   - Tier1 limit: glue ≤ 2 (vs dynamic TIER1)
   - Tier2 limit: glue ≤ 3 (vs dynamic TIER2)

---

## Results

### Performance Impact

| Configuration | Time | vs Baseline | Status |
|--------------|------|-------------|---------|
| Baseline (no optimization) | 2m52.75s | - | ✅ Reference |
| Aggressive fraction only | 2m54.12s | +1.37s slower | ❌ Worse |
| + Frequent reductions (500 init) | 2m57.56s | +4.81s slower | ❌ Much worse |
| + Very frequent (300 interval) | 2m58.26s | +5.51s slower | ❌ Even worse |

**Conclusion:** Aggressive learned clause deletion **HURTS** performance on f1.cnf by 1-6 seconds.

---

## Why It Failed

### Theory vs Reality

**Theory:** Massive learned clause database → slow propagation → aggressive deletion helps

**Reality for f1.cnf:**
1. **Encoding structure is self-pruning**: Binary/ternary clauses don't generate many useless learned clauses
2. **Glue heuristic works well**: Kissat's default glue-based deletion already keeps good clauses
3. **GC overhead dominates**: More frequent/aggressive deletion spends more time in garbage collection than we save in propagation
4. **Cache effects minor**: f1.cnf is memory-bound by value array size, not clause database size

### Profiling Evidence

From previous benchmarks:
- Default Kissat: 1.80M conflicts, processes efficiently
- Learned clauses don't dominate memory (value array is 1.8MB, clauses much smaller individually)
- Propagation rate: 8M props/sec - not clause-limited

---

## Lessons Learned

### When Aggressive Deletion Helps
✅ **Good for:**
- Random SAT instances with huge learned databases
- Crafted instances designed to fool CDCL
- Instances where learned:original ratio > 10:1
-Long-running searches (days/weeks)

❌ **Bad for:**
- Structured/encoding problems (like f1.cnf)
- Instances solvable in minutes/hours
- Problems with good clause learning (low glue clauses are valuable!)

### The Real Bottleneck

For f1.cnf, the bottleneck is **NOT** learned clause bloat. It's:
1. **Memory bandwidth** - 467K variable value array doesn't fit in cache
2. **Binary clause overhead** - 41% of clauses could be stored inline (see Optimization #1)
3. **Propagation instruction count** - each prop does bounds checks, indirect loads, etc.

---

## Recommendation

**REVERT** this optimization for f1.cnf-like problems.

Instead, focus on optimizations that address the real bottlenecks:
- **Optimization #1**: Inline binary clause storage (15-25% gain expected)
- **Optimization #4**: Value array compression (8-12% gain expected)
- **Optimization #3**: Watch list compaction of satisfied clauses (5-8% gain expected)

---

## Code Status

The aggressive reduction code is implemented but currently **reverted** to standard behavior:
- Fraction changes: Active (keeps 15-20% vs 50-90%) - **CAUSES SLOWDOWN**
- Frequency changes: Reverted to default
- Tier limit changes: Active (glue ≤2-3) - **CAUSES SLOWDOWN**

### To Fully Revert

Remove the large instance special cases from:
1. `src/reduce.c` lines ~106-126 (mark_less_useful_clauses_as_garbage)
2. `src/reduce.c` lines ~38-77 (collect_reducibles)

Or simply checkout the original files:
```bash
git checkout src/reduce.c src/kimits.c
```

---

## Next Steps

Based on f1.cnf analysis, implement these instead:

1. **Inline Binary Clause Storage** (Optimization #1)
   - Expected: 15-25% gain
   - Rationale: 41% of clauses are binary, eliminate arena indirection

2. **Value Array Compression** (Optimization #4)
   - Expected: 8-12% gain
   - Rationale: 467K vars = 1.8MB array, compress to 230KB

3. **Binary Watch List Compaction** (Optimization #3)
   - Expected: 5-8% gain
   - Rationale: Remove satisfied binary watches periodically

**Total Expected: 28-45% speedup from these three**

---

## Summary

Aggressive learned clause deletion was a **reasonable hypothesis** but **doesn't work for f1.cnf** because:
- The problem structure is well-behaved
- Default Kissat heuristics already work well
- GC overhead exceeds any propagation savings
- The real bottlenecks are elsewhere (memory bandwidth, binary clause overhead)

This is a valuable negative result - it tells us what NOT to optimize and redirects efforts to more promising approaches.
