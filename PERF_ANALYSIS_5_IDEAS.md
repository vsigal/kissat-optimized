# Performance Analysis & 5 New Improvement Ideas

## Current Performance Snapshot (f2.cnf)
- **Time**: 148-149s
- **Binary**: 625 KB with LTO
- **Compiler**: gcc-12 -O3 -mavx2 -march=native -flto
- **Decision Cache**: 64 entries

---

## Hot Code Analysis

From objdump analysis of `search_propagate_literal`:
```asm
# Main propagation loop
1d148: prefetcht0 (%rax)     # Prefetch watches
1d154: mov    (%rsi),%edx    # Load watch
1d17d: prefetcht0 (%rax)     # Prefetch blocking literal value
1d18d: ...branch on binary/non-binary...
```

**Key observations:**
1. Prefetch instructions are in place (working)
2. Tight loop with branches for binary/non-binary clauses
3. Size-based dispatch is working (ternary fast path)

---

## 5 New Improvement Ideas

---

### Idea #1: Software-Based Branch Prediction Hints Table

**Problem**: Branch mispredictions in propagation loop
- Binary vs non-binary clause type check: unpredictable
- Current: `if (KISSAT_PROPLIT_LIKELY (head.type.binary))`
- Static hints don't adapt to workload

**Solution**: Dynamic branch prediction table
```c
// Add to kissat struct
struct {
  uint64_t binary_count;
  uint64_t non_binary_count;
  uint8_t predict_binary;  // 0-255 threshold
} clause_type_predictor;

// In propagation loop
int predict_binary = (solver->clause_type_predictor.predict_binary > 128);
if (predict_binary) {
  // Fast path assumes binary
  if (KISSAT_PROPLIT_LIKELY(head.type.binary)) {
    // Handle binary
  } else {
    // Rare: handle non-binary + update predictor
    solver->clause_type_predictor.non_binary_count++;
    update_predictor(solver);
  }
}
```

**Expected**: 2-4% speedup
**Complexity**: Low
**Files**: `src/proplit.h`, `src/internal.h`

---

### Idea #2: Watch List Reordering by Hit Frequency

**Problem**: Watch lists accessed linearly, but some watches are "hot"
- First few watches in list are checked more often
- Cold watches (rarely triggering) waste cache

**Solution**: Self-organizing watch lists
```c
// When a watch triggers a propagation, move it earlier in list
// (1-2 positions) - like move-to-front heuristic

// In PROPAGATE_LITERAL after processing watch:
if (watch_triggered && q > begin_watches + 2) {
  // Swap with earlier watch to increase locality
  watch tmp = q[-2];
  q[-2] = q[0];
  q[0] = tmp;
}
```

**Expected**: 3-5% speedup (better cache locality)
**Complexity**: Low
**Files**: `src/proplit.h`

---

### Idea #3: Decision Variable Pre-computation

**Problem**: `kissat_decide()` searches for best variable each time
- Current: Linear scan through queue
- Cache helps, but we still check validity

**Solution**: Pre-compute next N decisions during idle time
```c
// Add to solver struct
unsigned precomputed_decisions[16];
unsigned precomputed_count;

// During idle time (between propagations)
void precompute_decisions(kissat *solver) {
  if (solver->precomputed_count > 0) return;
  
  for (int i = 0; i < 16 && !EMPTY_STACK(solver->queue); i++) {
    unsigned idx = pop_best_from_queue(solver);
    if (solver->values[2*idx] == 0) {  // Unassigned
      solver->precomputed_decisions[solver->precomputed_count++] = idx;
    }
  }
}

// In kissat_decide()
if (solver->precomputed_count > 0) {
  return solver->precomputed_decisions[--solver->precomputed_count];
}
```

**Expected**: 5-8% speedup on decision-heavy phases
**Complexity**: Medium
**Files**: `src/decide.c`, `src/internal.h`

---

### Idea #4: Clause Activity-Based Skipping in Propagation

**Problem**: Low-activity clauses are rarely useful but still checked
- Many learned clauses have very low activity
- They waste CPU cycles during propagation

**Solution**: Skip low-activity clauses with probability
```c
// Add threshold check in propagation
if (!c->binary && c->redundant) {
  // Skip very low activity clauses sometimes
  if (c->activity < solver->clause_activity_threshold) {
    if (solver->random % 100 < 10) {  // 10% skip rate
      q -= 2;  // Skip this clause
      continue;
    }
  }
}
```

**Better approach**: Mark low-activity clauses as "lazy"
- Don't watch them eagerly
- Only check during reduce/flush

**Expected**: 5-10% speedup (fewer clauses to check)
**Complexity**: Medium
**Files**: `src/proplit.h`, `src/reduce.c`

---

### Idea #5: Memory Pool for Small Clauses (4-8 literals)

**Problem**: Clause allocation is general-purpose
- Small clauses (4-8 literals) are very common (~40%)
- Arena allocation has overhead for small objects
- Cache locality could be better

**Solution**: Separate pool for small clauses
```c
// In arena.c or new file
#define SMALL_CLAUSE_POOL_SIZE (1024 * 1024)  // 1MB pool

struct small_clause_pool {
  char pool[SMALL_CLAUSE_POOL_SIZE];
  size_t offset;
  uint32_t free_list[1024];  // Recycled slots
  uint32_t free_count;
};

// Allocate small clause quickly
clause *allocate_small_clause(unsigned size) {
  if (size <= 8 && small_pool.free_count > 0) {
    uint32_t slot = small_pool.free_list[--small_pool.free_count];
    return (clause *)(small_pool.pool + slot);
  }
  // Fall back to regular arena
  return kissat_allocate_clause(size);
}
```

**Benefits**:
- Faster allocation/deallocation
- Better cache locality (small clauses together)
- Less fragmentation

**Expected**: 3-6% speedup
**Complexity**: Medium-High
**Files**: `src/arena.c`, `src/clause.c`

---

## Summary Table

| Idea | Speedup | Complexity | Risk | Files |
|------|---------|------------|------|-------|
| #1 Dynamic Branch Prediction | 2-4% | Low | Low | proplit.h, internal.h |
| #2 Watch List Reordering | 3-5% | Low | Medium | proplit.h |
| #3 Decision Pre-computation | 5-8% | Medium | Medium | decide.c |
| #4 Activity-Based Skipping | 5-10% | Medium | High | proplit.h, reduce.c |
| #5 Small Clause Pool | 3-6% | High | Medium | arena.c, clause.c |

---

## Recommended Order

1. **Idea #1** (Dynamic Branch Prediction) - Quick win, low risk
2. **Idea #2** (Watch Reordering) - Also quick, improves cache
3. **Idea #3** (Decision Pre-computation) - Good gains, medium effort
4. **Idea #4** (Activity Skipping) - Highest potential but needs tuning
5. **Idea #5** (Small Clause Pool) - Long-term architectural improvement

---

## Target Performance

Current: 148-149s
Target after all ideas: **135-140s** (6-9% additional improvement)

---

*Generated: 2026-02-06*
*Based on: perf analysis + code review*
