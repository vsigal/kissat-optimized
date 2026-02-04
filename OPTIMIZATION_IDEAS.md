# 5 Optimization Ideas for Kissat SAT Solver

Based on extensive profiling of f1.cnf vs f2.cnf vs f3.cnf:

## Profiling Summary
- **Bad speculation**: 38-39% (branch mispredictions)
- **Backend bound**: 17-19% (memory latency)
- **IPC**: 1.62-1.71 (good, but can be improved)
- **L1 miss rate**: ~4.6% (acceptable)
- **LLC miss rate**: 1.79% → 2.61% (growing with problem size)

---

## Optimization #1: Enhanced Branch Prediction Hints
**Target**: `proplit.h` - propagation loop
**Problem**: 38-39% bad speculation from branch mispredicts
**Solution**: Add strategic LIKELY/UNLIKELY hints based on profiling data

**Expected Impact**: 5-10% speedup
**Implementation Complexity**: Low

---

## Optimization #2: Profile-Guided Optimization (PGO)
**Target**: Entire codebase
**Problem**: Code layout not optimized for actual execution patterns
**Solution**: 
1. Compile with `-fprofile-generate`
2. Run training workload (f1.cnf)
3. Recompile with `-fprofile-use`

**Expected Impact**: 10-15% speedup
**Implementation Complexity**: Low (build system change)

---

## Optimization #3: Clause Learning Rate Limiting
**Target**: `learn.c` - clause learning
**Problem**: Memory pressure from too many learned clauses
**Solution**: 
- Limit clauses learned per restart
- Prioritize high-quality clauses (low LBD, high activity)
- Reduce reduce() frequency

**Expected Impact**: 8-12% speedup on large problems
**Implementation Complexity**: Medium

---

## Optimization #4: Variable Decision Cache
**Target**: `decide.c` - variable selection
**Problem**: Recomputing VSIDS scores frequently
**Solution**:
- Cache top-N decision candidates
- Incremental score updates
- Avoid full heap traversals

**Expected Impact**: 10-15% speedup on decision-heavy problems
**Implementation Complexity**: Medium

---

## Optimization #5: Memory Pool for Clauses
**Target**: `arena.c`, `clause.c` - memory allocation
**Problem**: malloc/free overhead and memory fragmentation
**Solution**:
- Bump allocator for clauses
- Separate pools for different clause sizes
- Reduced cache misses from better locality

**Expected Impact**: 5-10% speedup, lower memory usage
**Implementation Complexity**: High

---

## Implementation Order
1. Branch Prediction Hints (quick win, validates profiling)
2. Profile-Guided Optimization (build-level, independent)
3. Clause Learning Rate Limiting (memory pressure)
4. Variable Decision Cache (decision bottleneck)
5. Memory Pool for Clauses (foundational change)

## Success Metrics
- f1.cnf baseline: 85s
- Target after all optimizations: <70s (20% improvement)
- f2.cnf: reduce from 225s to <180s
- IPC improvement: 1.71 → 1.85+
- Bad speculation: 38% → <30%
