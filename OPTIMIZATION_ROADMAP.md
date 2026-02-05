# Optimization Roadmap for Kissat on Mallob Cluster

**Target:** Single-core performance (1 process per core, no NUMA)  
**Hardware:** Intel & AMD Gen 4.5 (Zen 4) with AVX2  
**Current Status:** AVX2 implemented, debugging segfault

---

## Completed Optimizations

| # | Optimization | Speedup | Status |
|---|--------------|---------|--------|
| 1 | Software Prefetching | -9.6% time | ✅ Done |
| 2 | Branch Prediction Hints | -1.2% time | ✅ Done |
| 3 | Decision Cache (8-entry LRU) | -24.4% time | ✅ Done |
| 4 | **AVX2 SIMD** | TBD | ✅ Implemented, debugging |

---

## Planned Optimizations (Priority Order)

### #2: Clause Size Specialization
**Expected Speedup:** 5-10%  
**Complexity:** Medium  
**Files:** `src/proplit.h`  

**Problem:** Runtime size checks in hot loops  
**Solution:** Function pointer table dispatch

```c
// Add to proplit.h
typedef clause *(*clause_handler_t)(kissat *, clause *, unsigned);

static const clause_handler_t clause_handlers[] = {
  [2] = handle_binary_clause,
  [3] = handle_ternary_clause,
  [4] = handle_small_clause,
  [5] = handle_small_clause,
  [6] = handle_small_clause,
  [7] = handle_small_clause,
  [8] = handle_small_clause,
};

static inline clause *process_clause(kissat *solver, clause *c, unsigned not_lit) {
  if (c->size <= 8)
    return clause_handlers[c->size](solver, c, not_lit);
  return handle_large_clause(solver, c, not_lit);
}
```

**Why it works:**
- 70% of clauses are binary or ternary
- Eliminates size checks in hot loops
- Better branch prediction (size known at call site)
- 39.4% → ~30% bad speculation

---

### #3: Propagation Tick Budgeting
**Expected Speedup:** 6-10%  
**Complexity:** Medium  
**Files:** `src/proplit.h`, `src/search.c`  

**Problem:** All propagations counted equally, but costs vary:
- Binary: ~1 tick
- Large: 10+ ticks with scanning

**Solution:** Tick-based scheduling

```c
#define MAX_PROPAGATION_TICKS_PER_DECISION 10000

static inline clause *PROPAGATE_LITERAL_OPTIMIZED(kissat *solver, unsigned lit) {
  unsigned ticks = 0;
  
  // Phase 1: Binary watches (fast, cache-friendly)
  for (all_binary_watches(watch, watches)) {
    if (ticks++ > MAX_PROPAGATION_TICKS_PER_DECISION)
      goto defer_large_clauses;
    // ... process ...
  }
  
  // Phase 2: Ternary watches
  for (all_ternary_watches(watch, watches)) {
    if (ticks++ > MAX_PROPAGATION_TICKS_PER_DECISION)
      goto defer_large_clauses;
    // ... process ...
  }
  
  // Phase 3: Large clauses (deferred if over budget)
defer_large_clauses:
  // Process remaining or defer to next decision
}
```

**Why it works:**
- Processes similar-sized clauses together (cache locality)
- Prevents expensive clauses from blocking fast ones
- Reduces branch mispredictions

---

### #4: Conflict Analysis Batching
**Expected Speedup:** 8-12%  
**Complexity:** Medium  
**Files:** `src/analyze.c`, `src/bump.c`  

**Problem:** Conflict analysis has unpredictable branches, poor cache locality

**Solution:** Batch 4 conflicts, process together

```c
#define CONFLICT_BATCH_SIZE 4

struct conflict_batch {
  clause *conflicts[CONFLICT_BATCH_SIZE];
  unsigned count;
};

void kissat_process_conflict_batch(kissat *solver) {
  if (solver->conflict_batch.count < CONFLICT_BATCH_SIZE)
    return;
  
  // Batch-process:
  // - Single pass over analyzed[] array
  // - Combined bump_variable calls
  // - Vectorized score updates
  
  for (unsigned i = 0; i < CONFLICT_BATCH_SIZE; i++) {
    clause *c = solver->conflict_batch.conflicts[i];
    // ... analyze ...
  }
  
  kissat_batch_bump_variables(solver);  // Cache-friendly score updates
}
```

**Why it works:**
- Better cache locality for analyzed[] array
- Reduced branch mispredictions
- Vectorizable score updates

---

### #5: Temporal Clause Partitioning
**Expected Speedup:** 10-15%  
**Complexity:** High  
**Files:** `src/arena.c`, `src/collect.c`  

**Problem:** Clauses accessed randomly, poor cache locality  
**Metrics:** L1 miss rate 5.85%, backend bound 16.9%

**Solution:** Hot/warm/cold regions based on usage

```c
typedef struct arena_partitions {
  reference hot_start, hot_end;    // L1 resident (recent conflicts)
  reference warm_start, warm_end;  // L2 resident (past 1000 conflicts)
  reference cold_start, cold_end;  // Memory (not recently used)
} arena_partitions;

// Fast path for hot clauses
static inline clause *kissat_dereference_clause_fast(kissat *solver, reference ref) {
  if (ref < solver->partitions.hot_end)
    return (clause *) (solver->arena.begin + ref);
  return kissat_dereference_clause(solver, ref);
}

// Promote clause after use in conflict
void kissat_promote_clause(kissat *solver, clause *c) {
  if (is_hot(c)) return;
  reference new_ref = kissat_allocate_in_hot(solver, c->size);
  clause *new_c = kissat_dereference_clause(solver, new_ref);
  memcpy(new_c, c, kissat_bytes_of_clause(c->size));
  c->garbage = true;
}
```

**Why it works:**
- Hot clauses stay in L1 cache
- Reduces L1 miss rate: 5.85% → ~3%
- Reduces backend bound: 16.9% → ~10%

---

## Implementation Priority

| Rank | Idea | Speedup | Risk | Effort |
|------|------|---------|------|--------|
| 1 | #2 Size Specialization | 5-10% | Low | Medium |
| 2 | #3 Tick Budgeting | 6-10% | Medium | Medium |
| 3 | #4 Conflict Batching | 8-12% | Medium | Medium |
| 4 | #5 Temporal Partitioning | 10-15% | High | High |

**Recommended Order:**
1. ✅ #1 AVX2 (done, debug segfault)
2. #2 Size Specialization (good ROI, low risk)
3. #3 Tick Budgeting (if more needed)
4. #4 Conflict Batching (for max gains)
5. #5 Temporal Partitioning (last resort, high effort)

---

## Success Metrics

**Target:** f2.cnf = 153.3s → ~120s (22% additional speedup)  
**Cumulative:** 235s → 120s = 49% total speedup

**Key Metrics to Track:**
- Wall clock time (primary)
- IPC (instructions per cycle)
- Backend bound %
- Bad speculation %
- L1/L2/LLC miss rates

---

## Testing Protocol

For each optimization:
1. Implement with feature flag
2. Test on f2.cnf (5 runs, median)
3. Verify correctness (no SAT/UNSAT change)
4. Measure with `perf stat`
5. Document speedup
6. Commit if >3% improvement

---

**Last Updated:** 2026-02-05  
**Next Milestone:** Fix AVX2 segfault, benchmark, implement #2
