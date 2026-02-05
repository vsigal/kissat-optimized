# Quick Start - Next Session

## 🚨 IMMEDIATE TASK: Fix Segfault

```bash
cd /home/vsigal/src/kissat

# Run debug script
bash debug_segfault.sh

# Or manually:
timeout 30 ./build/kissat f2.cnf 2>&1 | tail -10
# Should show: "c caught signal 11 (SIGSEGV)"

# Find cause with GDB
cd build
gdb ./kissat
(gdb) run ../f2.cnf
# ... wait for crash ...
(gdb) bt  # <-- This shows where it crashes
```

## ✅ What's Working
- AVX2 implementation complete
- CPU detection working (`AVX2=yes`)
- Build system configured

## ⚠️ What's Broken
- Segfault at end of solving
- Likely cause: Out-of-bounds gather or buffer overflow

## 📋 Files Modified
```
src/simdscan.c     - AVX2 implementation (main work)
src/application.c  - Added SIMD init call
src/internal.c     - Removed duplicate init
```

## 🔧 Quick Fixes to Try

### Option 1: Add bounds checking to gather
Edit `src/simdscan.c`, in `avx2_find_non_false()`:
```c
// Before gather, check literals are in bounds
for (int j = 0; j < 8; j++) {
  if (lits[i + j] >= solver->size_values) {
    // Handle out of bounds
  }
}
```

### Option 2: Disable SIMD temporarily
Comment out AVX2 path in `kissat_simd_find_non_false()`:
```c
// #if KISSAT_HAS_AVX2
// if (cpu_features.avx2) { ... }
// #endif
```

### Option 3: Check arena alignment
Verify clause arena is properly aligned for AVX2 loads.

## 📊 Benchmark Once Fixed
```bash
# Run 3 times, take median
time ./build/kissat f2.cnf > /dev/null 2>&1
time ./build/kissat f2.cnf > /dev/null 2>&1
time ./build/kissat f2.cnf > /dev/null 2>&1
```

## 📚 Full Documentation
- `SESSION_CONTINUE.md` - Complete session guide
- `OPTIMIZATION_ROADMAP.md` - 4 more optimizations planned
- `debug_segfault.sh` - Automated debug script

## 🎯 Goal
Fix segfault → Benchmark → Implement optimization #2 (Size Specialization)

---
**Ready for new session!**
