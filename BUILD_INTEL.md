# Simple Build Instructions for Intel

## Option 1: Intel Skylake / Skylake-X (AVX2 + AVX-512)

### For Skylake-X (HEDT/Server with AVX-512):

```bash
cd /home/vsigal/src/kissat
rm -rf build && mkdir build && cd build

# Configure with ALL AVX-512 instructions
CFLAGS="-O3 \
  -march=skylake-avx512 \
  -mavx512f -mavx512bw -mavx512vl -mavx512cd -mavx512dq \
  -mavx512vpopcntdq -mavx512bitalg \
  -mavx512vbmi -mavx512vbmi2 \
  -mavx512vnni \
  -mgfni -mvaes -mvpclmulqdq \
  -flto \
  -funroll-loops \
  -DNDEBUG \
  -DKISSAT_HAS_AVX512=1 \
  -DKISSAT_HAS_AVX512_BITOPS=1 \
  -DKISSAT_HAS_GFNI=1" \
LDFLAGS="-flto" \
../configure

make -j$(nproc)
```

### For Skylake (Client, AVX2 only):

```bash
cd /home/vsigal/src/kissat
rm -rf build && mkdir build && cd build

# Configure with AVX2
CFLAGS="-O3 \
  -march=skylake \
  -mavx2 \
  -mfma \
  -mbmi -mbmi2 \
  -mf16c \
  -mfsgsbase \
  -flto \
  -funroll-loops \
  -DNDEBUG" \
LDFLAGS="-flto" \
../configure

make -j$(nproc)
```

---

## Option 2: Intel Sapphire Rapids / Emerald Rapids (Latest)

```bash
cd /home/vsigal/src/kissat
rm -rf build && mkdir build && cd build

# Configure with FULL AVX-512 (same as AMD Genoa)
CFLAGS="-O3 \
  -march=sapphirerapids \
  -mavx512f -mavx512bw -mavx512vl -mavx512cd -mavx512dq \
  -mavx512vpopcntdq -mavx512bitalg \
  -mavx512vbmi -mavx512vbmi2 \
  -mavx512vnni -mavx512bf16 \
  -mgfni -mvaes -mvpclmulqdq \
  -flto \
  -funroll-loops \
  -DNDEBUG \
  -DKISSAT_HAS_AVX512=1 \
  -DKISSAT_HAS_AVX512_BITOPS=1 \
  -DKISSAT_HAS_GFNI=1" \
LDFLAGS="-flto" \
../configure

make -j$(nproc)
```

---

## Option 3: Generic Intel (Auto-detect)

```bash
cd /home/vsigal/src/kissat
rm -rf build && mkdir build && cd build

# Let compiler auto-detect CPU features
CFLAGS="-O3 -march=native -DNDEBUG" \
../configure

make -j$(nproc)
```

---

## Quick Test

```bash
./kissat -q ../f1.cnf
```

---

## Which `-march` to use?

| CPU | `-march` | AVX-512 |
|-----|----------|---------|
| Skylake (client) | `skylake` | No |
| Skylake-X | `skylake-avx512` | Yes |
| Cascade Lake | `cascadelake` | Yes |
| Cooper Lake | `cooperlake` | Yes |
| Ice Lake | `icelake-server` | Yes |
| Sapphire Rapids | `sapphirerapids` | Yes |
| Emerald Rapids | `sapphirerapids` | Yes |

---

## Quick Copy-Paste (Detect your CPU)

```bash
cd /home/vsigal/src/kissat
rm -rf build && mkdir build && cd build

# Auto-detect with native
CFLAGS="-O3 -march=native -DNDEBUG" ../configure

make -j$(nproc)
./kissat -q ../f1.cnf
```
