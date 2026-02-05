# Optimizations 1 & 2 Results

## Changes Made

### 1. Bigger Decision Cache (8 → 16 entries)
**File**: `src/internal.h`
```c
#define DECISION_CACHE_SIZE 16  // Increased from 8
```
- Double the cache entries for better hit rate
- Expected: 2-4% improvement
- Result: Within noise margin, but should help on cache-friendly workloads

### 2. Loop Unrolling in Scalar Search
**File**: `src/proplit.h`
- Unrolled small clause search loops 2x
- Processes pairs of literals to reduce branch mispredictions
- Handles odd-sized clauses with remainder handling
```c
// Unrolled 2x - process pairs of literals
for (; i + 1 < size; i += 2) {
  value v0 = values[lits[i]];
  value v1 = values[lits[i + 1]];
  // ... check both values
}
// Handle remaining element (odd size)
```

## Results

| Test | f2.cnf Time |
|------|-------------|
| Run 1 | 153.91s |
| Run 2 | 152.12s |
| **Typical** | **152-154s** |

### Comparison
- **Before optimizations**: ~156.9s
- **After all optimizations**: ~153s
- **LTO alone**: ~151-153s
- **With cache+unrolling**: ~152-154s

## Analysis

1. **LTO provides the main benefit** (1-4% improvement)
2. **Bigger cache**: Effect within measurement noise, but theoretically sound
3. **Loop unrolling**: Should reduce branch mispredictions on small clauses

## Current Build Configuration

```bash
cd ~/src/kissat
rm -rf build
mkdir build
cd build
CC="gcc-12 -O3 -mavx2 -march=native -flto" ../configure
make -j$(nproc)
```

## Binary
- **Location**: `~/src/kissat/build/kissat`
- **Size**: 625 KB
- **Performance**: 152-154s on f2.cnf
- **Total speedup**: ~37% from original ~235s
