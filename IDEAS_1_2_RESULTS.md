# Ideas 1 & 2 Implementation Results

## Summary
Both ideas were attempted but did not yield performance improvements.

---

## Idea 1: Dynamic Branch Prediction

### Implementation
Added adaptive predictor for binary vs non-binary clause types:
```c
struct {
  uint64_t binary_count;
  uint64_t non_binary_count;
  uint8_t threshold;
  uint8_t counter;
  bool predict_binary;
} clause_type_predictor;
```

With macros to update predictor:
```c
#define UPDATE_PREDICTOR_BINARY(solver) ...
#define UPDATE_PREDICTOR_NON_BINARY(solver) ...
```

### Result
- **Performance**: 151-152s (slower than 149s baseline)
- **Status**: ❌ Reverted

### Why It Failed
1. **Modern CPUs have excellent branch predictors** - Hardware prediction is already very good
2. **Software overhead** - Updating counters on every clause adds instructions
3. **Static prediction works** - The `__builtin_expect` hints are already optimal

### Lesson
Don't compete with hardware branch prediction on modern CPUs.

---

## Idea 2: Watch List Reordering

### Implementation
Attempted move-to-front heuristic for hot watches:
```c
// After clause triggers propagation:
if (q > begin_watches + 4) {
  watch tmp = q[-2];
  q[-2] = q[-4];
  q[-4] = tmp;
  tmp = q[-1];
  q[-1] = q[-3];
  q[-3] = tmp;
}
```

### Result
- **Performance**: Segfault (crash)
- **Status**: ❌ Reverted

### Why It Failed
1. **Watch list structure is complex** - Non-binary watches are paired (head, tail)
2. **Swapping corrupts invariants** - The simple swap broke watch list consistency
3. **Pointer arithmetic errors** - q[-4] access was unsafe in some cases

### Correct Implementation Would Require
- Understanding watch pairing (head + tail for non-binary)
- Swapping complete watch pairs, not individual entries
- More careful bounds checking
- Potential reorganization of watch list layout

### Lesson
Watch list reordering is complex due to data structure invariants. A correct implementation would require significant refactoring.

---

## Final Status

### Current Best Performance
- **f2.cnf**: 148-149s
- **Configuration**: LTO + 64-entry decision cache + size specialization
- **Binary**: ~/src/kissat/build/kissat (625 KB)

### Ideas That Worked
1. ✅ LTO (Link-Time Optimization) - 1-4% improvement
2. ✅ Decision cache 8→64 entries - 2-3% improvement
3. ✅ Size specialization - 2-3% improvement

### Ideas That Didn't Work
1. ❌ Dynamic branch prediction - overhead > benefit
2. ❌ Watch list reordering - too complex, crashes
3. ❌ Tick budgeting - disrupted solver heuristics
4. ❌ Conflict batching - sequential dependencies
5. ❌ PGO - training issues

---

## Next Steps

Try ideas 3-5 from PERF_ANALYSIS_5_IDEAS.md:
1. **Idea 3**: Decision pre-computation (5-8% expected)
2. **Idea 4**: Activity-based clause skipping (5-10% expected)
3. **Idea 5**: Small clause memory pool (3-6% expected)

Or try completely different approaches:
- Different compiler (clang instead of gcc)
- Profile-guided feedback from actual workload
- Algorithmic changes (restart strategies, variable selection)

---

*Last updated: 2026-02-06*