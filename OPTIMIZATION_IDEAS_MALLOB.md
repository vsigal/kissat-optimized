# 5 Optimization Ideas for Mallob Cluster Deployment

## Current Performance Baseline (f2.cnf)
- **Wall clock**: 153.3 seconds
- **Process time**: 153.2 seconds
- **IPC**: 1.61
- **Backend bound**: 16.9%
- **Bad speculation**: 39.4%
- **L1 miss rate**: 5.85%
- **LLC miss rate**: 4.72%

**Note**: For Mallob (1 process/core), NUMA is irrelevant. Focus on:
1. Single-threaded performance
2. Cache efficiency
3. Branch prediction
4. Memory bandwidth per core

---

## Idea #1: Lock-Free Conflict Analysis Batching

**Problem**: Conflict analysis uses stack operations that can be batched

**Observation**: From perf data, we see:
- 1,427,993 conflicts
- 100M propagations
- Bad speculation at 39.4%

The conflict analysis loop has unpredictable branches when traversing the implication graph.

**Solution**: Batch conflict analysis for multiple conflicts

```c
// Instead of analyzing each conflict immediately,
// collect N conflicts and process them together
#define CONFLICT_BATCH_SIZE 4

struct conflict_batch {
  clause *conflicts[CONFLICT_BATCH_SIZE];
  unsigned count;
};

// Process batch when full or at decision boundary
void kissat_process_conflict_batch(kissat *solver) {
  if (solver->conflict_batch.count < CONFLICT_BATCH_SIZE)
    return;
  
  // Batch-process: better cache locality
  // - Single pass over analyzed[] array
  // - Combined bump_variable calls
  // - Vectorized score updates
  
  for (unsigned i = 0; i < CONFLICT_BATCH_SIZE; i++) {
    clause *c = solver->conflict_batch.conflicts[i];
    // ... analyze ...
  }
  
  // Batch update scores - better for cache
  kissat_batch_bump_variables(solver);
}
```

**Expected Benefit**:
- 8-12% speedup
- Reduces branch mispredictions in analysis
- Better cache locality for score updates
- 39.4% → ~32% bad speculation

**Complexity**: Medium (changes analyze.c, bump.c)

---

## Idea #2: Temporal Clause Partitioning

**Problem**: Clause traversal during propagation is cache-unfriendly

**Observation**: 
- L1 miss rate: 5.85%
- Backend bound: 16.9%
- Clauses are accessed randomly based on watched literals

**Solution**: Partition clauses by "temperature" (recent usage)

```c
// Split arena into hot/warm/cold regions
typedef struct arena_partitions {
  // Hot: Recently used in conflicts (L1 resident)
  reference hot_start, hot_end;
  
  // Warm: Used in past 1000 conflicts (L2 resident)
  reference warm_start, warm_end;
  
  // Cold: Not recently used (memory)
  reference cold_start, cold_end;
} arena_partitions;

// During propagation, prioritize hot clauses
static inline clause *kissat_dereference_clause_fast(kissat *solver, reference ref) {
  // Inline the common case for hot clauses
  if (ref < solver->partitions.hot_end)
    return (clause *) (solver->arena.begin + ref);
  
  // Cold path
  return kissat_dereference_clause(solver, ref);
}

// Promote clauses after use in conflict
void kissat_promote_clause(kissat *solver, clause *c) {
  if (is_hot(c)) return;
  
  // Move to hot region (copy semantics)
  reference new_ref = kissat_allocate_in_hot(solver, c->size);
  clause *new_c = kissat_dereference_clause(solver, new_ref);
  memcpy(new_c, c, kissat_bytes_of_clause(c->size));
  
  // Mark old as garbage
  c->garbage = true;
}
```

**Expected Benefit**:
- 10-15% speedup
- L1 miss rate: 5.85% → ~3%
- Backend bound: 16.9% → ~10%

**Complexity**: Medium-High (changes arena.c, collect.c)

---

## Idea #3: SIMD-Accelerated Literal Scanning with AVX2

**Problem**: Large clause scanning is slow; current SIMD only for AVX-512

**Current Code**: `simdscan.c` has AVX-512 implementation, but AVX2 is stubbed out

**Solution**: Full AVX2 implementation for broad hardware support

```c
// In simdscan.c - AVX2 implementation
#if KISSAT_HAS_AVX2
static inline bool avx2_find_non_false(const value *values,
                                        const unsigned *lits,
                                        size_t start_idx,
                                        size_t end_idx,
                                        unsigned *out_replacement) {
  // AVX2: 256-bit registers = 32 bytes = 32 literals
  const size_t simd_width = 32;
  
  for (size_t i = start_idx; i + simd_width <= end_idx; i += simd_width) {
    // Gather 32 literal values
    // AVX2 doesn't have gather_epi8, so we use scalar loads
    // But we can process 32 in parallel with vectorized compare
    
    __m256i vals = _mm256_loadu_si256((__m256i *)(values + lits[i]));
    // ... mask operations to find non-false ...
    
    // Alternative: Use _mm256_shuffle_epi8 for table lookup
    // if values[] is small enough to fit in a pattern
  }
}
#endif
```

**Expected Benefit**:
- 5-8% speedup on large clauses
- Better utilization of modern CPUs (AVX2 more common than AVX-512)
- Especially helpful for f3/f4 with more learned clauses

**Complexity**: Low-Medium (extends existing simdscan.c)

---

## Idea #4: Propagation Tick Budgeting

**Problem**: Some propagations are more expensive than others, but all counted equally

**Observation**:
- Binary clauses: ~1 tick
- Large clauses: 10+ ticks with scanning
- Current code doesn't prioritize "cheap" propagations

**Solution**: Implement tick-based scheduling for propagation

```c
// In proplit.h - prioritize binary clauses

#define MAX_PROPAGATION_TICKS_PER_DECISION 10000

static inline clause *PROPAGATE_LITERAL_OPTIMIZED(kissat *solver, unsigned lit) {
  // Phase 1: Process all binary watches (fast, cache-friendly)
  // Binary clauses are compact and sequential
  for (all_binary_watches(watch, watches)) {
    // ... fast binary propagation ...
    if (ticks++ > MAX_PROPAGATION_TICKS_PER_DECISION)
      goto defer_large_clauses;
  }
  
  // Phase 2: Process ternary watches (medium cost)
  for (all_ternary_watches(watch, watches)) {
    // ... ternary propagation ...
    if (ticks++ > MAX_PROPAGATION_TICKS_PER_DECISION)
      goto defer_large_clauses;
  }
  
  // Phase 3: Large clauses (expensive, deferred if over budget)
  for (all_large_watches(watch, watches)) {
    // ... large clause propagation ...
  }
  
defer_large_clauses:
  // Mark remaining as deferred for next decision level
  // or process incrementally
}
```

**Expected Benefit**:
- 6-10% speedup
- Better cache utilization by processing similar-sized clauses together
- Reduces branch mispredictions

**Complexity**: Medium (changes proplit.h, search.c)

---

## Idea #5: Compile-Time Clause Size Specialization

**Problem**: Clause processing has runtime size checks that could be compile-time

**Observation**:
- 70% of clauses are binary or ternary
- Current code has `if (c->size == 2)` checks in hot loops
- C++ templates could specialize, but we're in C

**Solution**: Use function pointer tables for size-specific handlers

```c
// Clause size specialization table
typedef clause *(*clause_handler_t)(kissat *, clause *, unsigned);

static const clause_handler_t clause_handlers[] = {
  [2] = handle_binary_clause,      // Specialized
  [3] = handle_ternary_clause,     // Specialized  
  [4] = handle_small_clause,       // Generic but optimized
  [5] = handle_small_clause,
  [6] = handle_small_clause,
  [7] = handle_small_clause,
  [8] = handle_small_clause,
  // [9+] = handle_large_clause via default
};

static inline clause *process_clause(kissat *solver, clause *c, unsigned not_lit) {
  if (c->size <= 8) {
    // Direct dispatch - no bounds check needed
    return clause_handlers[c->size](solver, c, not_lit);
  }
  return handle_large_clause(solver, c, not_lit);
}

// Specialized handler for binary - fully unrolled, no loops
static clause *handle_binary_clause(kissat *solver, clause *c, unsigned not_lit) {
  // Unrolled: just 2 literals to check
  unsigned other = c->lits[0] ^ c->lits[1] ^ not_lit;
  value other_val = solver->values[other];
  
  if (other_val > 0) return NULL;  // Satisfied
  if (other_val < 0) return c;      // Conflict
  
  // Unit propagation
  kissat_fast_assign_reference(solver, solver->values, solver->assigned, 
                               other, INVALID_REF, c);
  return NULL;
}
```

**Expected Benefit**:
- 5-10% speedup
- Eliminates size checks in hot loops
- Better branch prediction (size known at call site)
- 39.4% → ~30% bad speculation

**Complexity**: Medium (changes proplit.h, adds dispatch table)

---

## Implementation Priority for Mallob

| Rank | Idea | Speedup | Complexity | Risk |
|------|------|---------|------------|------|
| 1 | #3 SIMD AVX2 | 5-8% | Low-Medium | Low |
| 2 | #5 Size Specialization | 5-10% | Medium | Low |
| 3 | #4 Tick Budgeting | 6-10% | Medium | Medium |
| 4 | #1 Conflict Batching | 8-12% | Medium | Medium |
| 5 | #2 Temporal Partitioning | 10-15% | High | High |

**Recommended Order**:
1. Start with #3 (AVX2) - easy win
2. Then #5 (Specialization) - good return for effort
3. #4 (Tick Budgeting) if more needed
4. #1 and #2 for maximum performance
