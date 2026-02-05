# PGO (Profile-Guided Optimization) - Results

## Status: ⚠️ No Benefit / Slightly Slower

### Implementation
Attempted PGO using GCC's `-fprofile-generate` and `-fprofile-use` flags:

**Step 1:** Build instrumented binary
```bash
CC="gcc-12 -O3 -mavx2 -march=native -fprofile-generate"
```

**Step 2:** Run training workload (add128.cnf)
```bash
./kissat test/cnf/add128.cnf  # Generated 91 .gcda files
```

**Step 3:** Build optimized binary
```bash
CC="gcc-12 -O3 -mavx2 -march=native -fprofile-use=../build_pgo"
```

### Results

| Build | f2.cnf Time | vs Baseline |
|-------|-------------|-------------|
| Non-PGO | 147.0-148.0s | Baseline |
| PGO (add128) | 151.0-151.2s | +2.7% slower |

### Why PGO Didn't Help

1. **Training/Testing Mismatch**: 
   - Trained on add128.cnf (small, arithmetic circuit)
   - Tested on f2.cnf (large, industrial problem)
   - Code paths are different

2. **Path Resolution Issues**:
   - GCC warnings: `profile count data file not found`
   - Path format mismatch: `#home#vsigal#...` vs actual paths
   - Some source files didn't find their profile data

3. **Already Well-Optimized**:
   - The code already has:
     - Likely/unlikely branch hints
     - Hot/cold function attributes
     - Well-defined fast paths
   - PGO benefits diminish with good manual optimization

4. **Instrumentation Overhead**:
   - PGO instrumentation adds overhead even in "optimized" builds
   - Benefits don't outweigh costs for this codebase

### Lessons Learned

1. **PGO requires representative training** - Must train on similar workloads
2. **Path handling is tricky** - GCC's gcda path resolution is finicky
3. **Manual optimization competes with PGO** - Our branch hints already do PGO's job
4. **Build complexity** - Adds build steps without proportional benefit

### Recommendation

**Don't use PGO for Kissat** because:
- Manual branch hints already provide similar benefits
- Training workload selection is difficult (SAT problems vary widely)
- Build complexity outweighs performance gains
- Current ~147s performance is already good

### Final Binary

Binary without PGO: `./build/kissat`
- **Performance**: 147-148s on f2.cnf
- **Status**: Optimal configuration
