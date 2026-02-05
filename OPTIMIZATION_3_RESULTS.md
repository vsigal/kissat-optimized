# Optimization #3: Tick Budgeting - Results

## Status: ⚠️ Reverted to Optimization #2 only

### Attempt
Implemented tick budgeting system with:
- Per-clause-type tick costs (binary=1, ternary=1, small=2, large=5)
- Budget thresholds (100/500/2000 ticks)
- Budget check before processing large clauses
- Deferral mechanism for clauses over budget

### Results
1. **With aggressive budget (break out of loop)**: 276s (86% slower)
   - Too many deferred clauses caused inefficiency
   - Breaking propagation loop early disrupted solver state

2. **With lenient budget (informational only)**: 171s (15% slower)
   - Tick accounting changes affected solver heuristics
   - Modified tick counts confused internal metrics

3. **Reverted to Opt #2 only**: 148s (baseline)
   - Restored original tick counting
   - Kept size specialization from Opt #2

### Lessons Learned
1. **Tick counting is fragile** - The solver uses ticks for internal heuristics
2. **Deferring clauses is expensive** - Breaking propagation loop adds overhead
3. **Simple optimizations work better** - Size specialization (Opt #2) is effective

### Final State
- ✅ Optimization #2 (Size Specialization) working: 2.8% speedup
- ❌ Optimization #3 (Tick Budgeting) reverted
- ✅ Binary in ./build/ ready for testing

### Performance Summary
| Version | f2.cnf Time | Status |
|---------|-------------|--------|
| Baseline | 153.3s | - |
| After Opt #2 | 148.0s | ✅ +2.8% |
| Opt #3 attempt | 171-276s | ❌ Reverted |

### Files
- Binary: `./build/kissat` (Optimization #2 only)
- Modified: `src/proplit.h`, `src/simdscan.c`
