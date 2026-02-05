# Next Performance Improvement Ideas

## Current Status
- **f2.cnf**: 153.9s (2.9% improvement from baseline 156.9s)
- **Total speedup**: 37% from original ~235s
- **Binary**: ~/src/kissat/build/kissat

---

## High Priority (Good ROI)

### 1. Link-Time Optimization (LTO)
**Effort**: Low | **Risk**: Low | **Expected**: 5-10%
```bash
CC="gcc-12 -O3 -mavx2 -march=native -flto" ./configure
```
- Enables cross-module inlining
- Better dead code elimination
- Whole-program optimization

### 2. Loop Unrolling in Propagation
**Effort**: Low | **Risk**: Low | **Expected**: 3-5%
- Unroll watch loop 2-4x in proplit.h
- Reduce loop overhead
- Better instruction scheduling

### 3. Bigger Decision Cache
**Effort**: Low | **Risk**: Low | **Expected**: 2-4%
- Current: 8-entry LRU
- Try: 16-entry or 32-entry
- File: src/decide.c

---

## Medium Priority (Moderate Effort)

### 4. Temporal Clause Partitioning (Opt #5 from roadmap)
**Effort**: High | **Risk**: Medium | **Expected**: 10-15%
- Hot/warm/cold clause regions
- Files: src/arena.c, src/collect.c
- Keep frequently used clauses in L1 cache

### 5. Hierarchical Watch Lists
**Effort**: Medium | **Risk**: Medium | **Expected**: 10-15%
- Separate arrays for binary/ternary/large watches
- Better cache locality
- Files: src/watch.h, src/proplit.h

### 6. Clause Learning Rate Limiting
**Effort**: Medium | **Risk**: Medium | **Expected**: 8-12%
- Limit clauses learned per restart
- Prioritize high-quality (low LBD) clauses
- File: src/learn.c

---

## Low Priority (High Effort/Risk)

### 7. Structure-of-Arrays (SoA)
**Effort**: Very High | **Risk**: High | **Expected**: 8-12%
- Separate hot/cold clause fields
- Touches 50+ files
- High risk of bugs

### 8. GPU Offloading
**Effort**: Very High | **Risk**: High | **Expected**: ?
- Offload propagation to GPU
- Only beneficial for very large problems
- Requires significant architecture changes

### 9. NUMA Awareness
**Effort**: Medium | **Risk**: Low | **Expected**: 10-20%
- Only for multi-socket systems
- Memory affinity optimization
- Not relevant for single-core per instance

---

## Recommended Next Steps

### Immediate (This Week)
1. **Try LTO** - One-line change, low risk
2. **Increase decision cache** - 8 → 16 entries

### Short Term (Next Month)
3. **Hierarchical Watch Lists** - Good potential, medium effort
4. **Loop unrolling** - Quick win in propagation

### Long Term (Future)
5. **Temporal Clause Partitioning** - Highest potential but high effort
6. **SoA** - Only if other optimizations exhausted

---

## Metrics to Track

For each optimization, measure:
- Wall clock time (primary)
- Instructions per cycle (IPC)
- Cache miss rates (L1, L2, LLC)
- Branch misprediction rate

Target: f2.cnf < 140s (10% additional improvement)

---

## Current Best Build

```bash
cd ~/src/kissat
rm -rf build
mkdir build
cd build
CC="gcc-12 -O3 -mavx2 -march=native" ../configure
make -j$(nproc)
./kissat f2.cnf  # Expected: ~154s
```

---

*Last updated: 2026-02-05*
