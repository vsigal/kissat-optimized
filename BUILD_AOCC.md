# Simple AOCC Build Instructions for AMD Genoa 4/5

## Step 1: Source AOCC Environment

```bash
# Source the AOCC environment (adjust path as needed)
source /opt/AMD/aocc-compiler/setenv_AOCC.sh

# Or if you have it in Downloads:
# source ~/Downloads/setenv_AOCC.sh
```

## Step 2: Set Compiler Variables

```bash
export CC=clang
export CXX=clang++
```

## Step 3: Configure with All AVX-512 Flags

```bash
cd /home/vsigal/src/kissat
rm -rf build && mkdir build && cd build

# Configure with ALL AVX-512 instructions for Genoa 4/5
CFLAGS="-O3 \
  -march=native \
  -mavx512f -mavx512bw -mavx512vl -mavx512cd -mavx512dq \
  -mavx512vpopcntdq -mavx512bitalg \
  -mavx512vbmi -mavx512vbmi2 \
  -mavx512vnni -mavx512bf16 \
  -mgfni -mvaes -mvpclmulqdq \
  -flto=thin \
  -funroll-loops \
  -finline-functions \
  -fvectorize \
  -fslp-vectorize \
  -DNDEBUG \
  -DKISSAT_HAS_AVX512=1 \
  -DKISSAT_HAS_AVX512_BITOPS=1 \
  -DKISSAT_HAS_GFNI=1" \
LDFLAGS="-z muldefs" \
../configure
```

## Step 4: Build

```bash
make -j$(nproc)
```

## Step 5: Verify SIMD Instructions

```bash
# Check that AVX-512 instructions are in the binary
objdump -d ./kissat | grep -E "vpcmpeq|vgather|vpbroadcast" | head -10

# Should show AVX-512 instructions
```

## Step 6: Test

```bash
./kissat -q ../f1.cnf
```

---

## Quick Copy-Paste Version

```bash
# 1. Source AOCC
source /opt/AMD/aocc-compiler/setenv_AOCC.sh

# 2. Set compilers
export CC=clang
export CXX=clang++

# 3. Build directory
cd /home/vsigal/src/kissat
rm -rf build && mkdir build && cd build

# 4. Configure with ALL AVX-512
CFLAGS="-O3 -march=native -mavx512f -mavx512bw -mavx512vl -mavx512cd -mavx512dq -mavx512vpopcntdq -mavx512bitalg -mavx512vbmi -mavx512vbmi2 -mavx512vnni -mavx512bf16 -mgfni -mvaes -mvpclmulqdq -flto=thin -funroll-loops -finline-functions -fvectorize -fslp-vectorize -DNDEBUG -DKISSAT_HAS_AVX512=1 -DKISSAT_HAS_AVX512_BITOPS=1 -DKISSAT_HAS_GFNI=1" LDFLAGS="-z muldefs" ../configure

# 5. Build
make -j$(nproc)

# 6. Test
./kissat -q ../f1.cnf
```

---

## Enabled Instructions Summary

| Instruction | Flag | Status |
|-------------|------|--------|
| AVX512F | `-mavx512f` | ✓ |
| AVX512BW | `-mavx512bw` | ✓ |
| AVX512VL | `-mavx512vl` | ✓ |
| AVX512CD | `-mavx512cd` | ✓ |
| AVX512DQ | `-mavx512dq` | ✓ |
| **AVX512_VPOPCNTDQ** | `-mavx512vpopcntdq` | ✓ |
| **AVX512_BITALG** | `-mavx512bitalg` | ✓ |
| AVX512_VBMI | `-mavx512vbmi` | ✓ |
| AVX512_VBMI2 | `-mavx512vbmi2` | ✓ |
| AVX512_VNNI | `-mavx512vnni` | ✓ |
| AVX512_BF16 | `-mavx512bf16` | ✓ |
| GFNI | `-mgfni` | ✓ |
| VAES | `-mvaes` | ✓ |
| VPCLMULQDQ | `-mvpclmulqdq` | ✓ |
