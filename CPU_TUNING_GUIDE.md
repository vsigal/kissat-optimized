# CPU Tuning Guide for Kissat SAT Solver

## Quick Reference for New CPUs

When testing on a new CPU (AMD Zen 4, Zen 5, Intel Sapphire Rapids, etc.):

---

## 1. Critical Parameters to Test

### A. Decision Cache Size (`src/internal.h`)
```c
#define DECISION_CACHE_SIZE 64  // Test: 8, 16, 32, 64, 128, 256
```

**Why it matters:**
- Larger L1/L2/L3 cache on newer CPUs → can handle bigger decision cache
- Zen 4: Try 64-128
- Zen 5: Try 64-256
- Intel Gen 4: Try 32-128

**Test procedure:**
```bash
for size in 16 32 64 128 256; do
  sed -i "s/DECISION_CACHE_SIZE [0-9]*/DECISION_CACHE_SIZE $size/" src/internal.h
  make clean && make -j$(nproc)
  time ./kissat f2.cnf > /dev/null 2>&1
done
```

---

### B. SIMD Width (Auto-detected, but verify)
```c
// In simdscan.c - automatically uses best available
// CPU will auto-detect: AVX-512 > AVX2 > SSE4.2 > scalar
```

**Verify detection:**
```bash
./kissat -v f2.cnf 2>&1 | grep -i "AVX\|SIMD"
# Should show: AVX-512F=yes/no, AVX2=yes/no, SSE4.2=yes/no
```

**If wrong detection:** Check CPU features in `/proc/cpuinfo`

---

### C. Prefetch Distance (`src/proplit.h`)
```c
#define WATCH_PREFETCH_DISTANCE 12  // Test: 8, 12, 16, 24
#define CLAUSE_PREFETCH_DISTANCE 4  // Test: 2, 4, 8
```

**Why it matters:**
- Larger cache lines on new CPUs → can prefetch further ahead
- Zen 4: Try 16-24
- Zen 5: Try 16-32
- Intel with L3$: Try 12-20

**Test procedure:**
```bash
for dist in 8 12 16 24; do
  sed -i "s/WATCH_PREFETCH_DISTANCE [0-9]*/WATCH_PREFETCH_DISTANCE $dist/" src/proplit.h
  make && time ./kissat f2.cnf > /dev/null 2>&1
done
```

---

### D. Compiler Flags (MOST IMPORTANT)

**AMD Zen 4:**
```bash
CC="gcc-12 -O3 -march=znver4 -flto" ./configure
```

**AMD Zen 5:**
```bash
CC="gcc-13 -O3 -march=znver5 -flto" ./configure
# or if gcc-13 not available:
CC="gcc-12 -O3 -march=znver4 -mtune=znver5 -flto" ./configure
```

**Intel Sapphire Rapids (Gen 4):**
```bash
CC="gcc-12 -O3 -march=sapphirerapids -flto" ./configure
```

**Intel Granite Rapids (Gen 5):**
```bash
CC="gcc-13 -O3 -march=graniterapids -flto" ./configure
```

**Generic (if specific arch unknown):**
```bash
CC="gcc-12 -O3 -march=native -flto" ./configure
```

---

## 2. CPU-Specific Optimizations

### AMD Zen 4 Specifics
- Native AVX-512 support (256-bit wide, good for 8-16 literals)
- Prefetch further ahead (16-24)
- Bigger decision cache (64-128)
- Branch predictor is very good - fewer likely/unlikely hints needed

### AMD Zen 5 Specifics
- Improved AVX-512 (faster 512-bit ops)
- Larger L1 cache (80KB vs 64KB) → can use 128-256 decision cache
- Better branch predictor
- Prefetch: 16-32 distance

### Intel Sapphire Rapids (Gen 4)
- AMX support (matrix operations - not useful for SAT)
- AVX-512 with good throughput
- Prefetch: 12-20 distance
- Decision cache: 64-128

### Intel Granite Rapids (Gen 5)
- Improved AVX-512
- Larger caches → 128-256 decision cache
- Prefetch: 16-24 distance

---

## 3. Test Protocol for New CPU

### Step 1: Baseline
```bash
cd ~/src/kissat
rm -rf build && mkdir build && cd build
CC="gcc-12 -O3 -march=native -flto" ../configure
make -j$(nproc)
time ./kissat f2.cnf > baseline.txt 2>&1
grep "process-time:" baseline.txt
```

### Step 2: Tune Decision Cache
```bash
cd ~/src/kissat
for size in 8 16 32 64 128 256; do
  echo "Testing cache size: $size"
  sed -i "s/#define DECISION_CACHE_SIZE [0-9]*/#define DECISION_CACHE_SIZE $size/" src/internal.h
  rm -rf build && mkdir build && cd build
  CC="gcc-12 -O3 -march=native -flto" ../configure
  make -j$(nproc)
  time ./kissat f2.cnf > /tmp/cache_${size}.txt 2>&1
  grep "process-time:" /tmp/cache_${size}.txt
  cd ..
done
```

### Step 3: Tune Prefetch Distance
```bash
# Find best cache size from step 2, then test prefetch
for dist in 8 12 16 24 32; do
  echo "Testing prefetch: $dist"
  sed -i "s/#define WATCH_PREFETCH_DISTANCE [0-9]*/#define WATCH_PREFETCH_DISTANCE $dist/" src/proplit.h
  # rebuild and test...
done
```

### Step 4: Compiler Optimization Level
```bash
# Test if -O3 is best, or try -Ofast
CC="gcc-12 -Ofast -march=native -flto" ./configure
```

---

## 4. Quick Performance Checklist

| Parameter | AMD Zen 4 | AMD Zen 5 | Intel Gen 4 | Intel Gen 5 |
|-----------|-----------|-----------|-------------|-------------|
| **-march** | znver4 | znver5 | sapphirerapids | graniterapids |
| **Decision Cache** | 64-128 | 128-256 | 64-128 | 128-256 |
| **Prefetch Dist** | 16-24 | 16-32 | 12-20 | 16-24 |
| **SIMD** | AVX2/AVX-512 | AVX2/AVX-512 | AVX-512 | AVX-512 |
| **GCC Version** | 12+ | 13+ | 12+ | 13+ |

---

## 5. Performance Measurement

Always measure multiple runs:
```bash
for i in 1 2 3; do
  time ./kissat f2.cnf > /dev/null 2>&1
done
```

Take the **median** of 3 runs (not average - outliers skew results).

---

## 6. Advanced: Per-CPU Compiler Flags

### AMD Zen 4
```bash
CFLAGS="-O3 -march=znver4 -mtune=znver4 -flto -fomit-frame-pointer \
        -falign-functions=32 -falign-loops=32"
```

### AMD Zen 5
```bash
CFLAGS="-O3 -march=znver5 -mtune=znver5 -flto -fomit-frame-pointer \
        -falign-functions=32 -falign-loops=32"
```

### Intel (latest)
```bash
CFLAGS="-O3 -march=sapphirerapids -flto -fomit-frame-pointer"
```

---

## 7. Verification

After tuning, verify:
```bash
# Check binary uses correct instructions
objdump -d ./kissat | grep -c "vmm"  # AVX-512 registers
objdump -d ./kissat | grep -c "ymm"  # AVX2 registers

# Run test
./kissat f2.cnf
# Should be 140-150s on modern CPU (f2.cnf)
```

---

## Summary

**Most important for new CPU:**
1. ✅ Use correct `-march` flag
2. ✅ Tune decision cache size (16-256)
3. ✅ Tune prefetch distance (8-32)
4. ✅ Use GCC 12+ for best optimizations
5. ✅ Always use `-flto`

**Expected gains on new CPU:**
- Zen 4 vs Zen 3: 10-20% faster
- Zen 5 vs Zen 4: 15-25% faster
- Intel Gen 4 vs Gen 3: 10-15% faster
- Intel Gen 5 vs Gen 4: 15-25% faster

---

*Last updated: 2026-02-05*