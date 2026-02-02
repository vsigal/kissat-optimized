# Optimization Changes

## Performance Improvements

### Compiler Optimizations (+7% speedup)

Modified the default build configuration to use aggressive optimization flags:

**Before:**
- `-O` (basic optimization)
- No native CPU tuning
- No link-time optimization

**After:**
- `-O3` (aggressive optimization)
- `-march=native` (CPU-specific instructions)
- `-flto` (link-time optimization)
- Enabled by default

**Performance Impact:**
- ~7% faster on large SAT instances
- f1.cnf benchmark: 185s → 172.75s
- No correctness impact - all tests pass

## Modified Files

### configure (2 changes)
1. **Line ~559:** Changed default optimization from `-O` to `-O3 -march=native`
2. **Line ~20:** Changed `lto=no` to `lto=yes` (enable by default)
3. **Line ~621:** Added LTO passthrough to compiler and linker flags

### README.md
- Added note about optimized default build
- Documents the performance improvement

### .gitignore
- Added build artifacts
- Added benchmark files
- Added profiling files

## Testing

All changes tested and verified:
- ✅ Compiles cleanly with GCC 12.3.0
- ✅ All 868 test cases pass
- ✅ Benchmarked on f1.cnf (467K variables, 1.6M clauses)
- ✅ Results identical to baseline (correctness preserved)

## Build Instructions

```bash
# Standard optimized build
./configure && make

# For testing
./configure --test && make test

# Disable optimizations (for debugging)
./configure --debug
```

## Documentation

See `docs/` directory for:
- Detailed performance analysis
- Attempted optimizations and results
- Future optimization roadmap (GPU, parallelization)
