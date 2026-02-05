#include "simdscan.h"
#include "inline.h"
#include "internal.h"
#include "print.h"
#include "report.h"

#include <immintrin.h>
#include <string.h>

// Runtime detection of CPU features
static struct {
  bool avx512f;
  bool avx512vl;
  bool avx512bw;
  bool avx512vbmi;
  bool avx512vpopcntdq;
  bool avx512bitalg;
  bool gfni;
  bool avx2;
  bool sse42;
  bool initialized;
} cpu_features = {0};

// CPUID helper - use inline assembly for portability
static void cpuid (int info[4], int function_id) {
#if defined(__x86_64__) || defined(__i386__)
#if defined(__GNUC__)
  __asm__ __volatile__ (
    "cpuid"
    : "=a" (info[0]), "=b" (info[1]), "=c" (info[2]), "=d" (info[3])
    : "a" (function_id), "c" (0)
  );
#else
  __cpuid (function_id, info[0], info[1], info[2], info[3]);
#endif
#else
  info[0] = info[1] = info[2] = info[3] = 0;
#endif
}

void kissat_init_simd_support (kissat *solver) {
  (void) solver;
  
  if (cpu_features.initialized)
    return;
  
  // First-time initialization

  
#if defined(__x86_64__) && defined(__GNUC__)
  int info[4];
  
  // Check max function ID
  cpuid (info, 0);
  int max_id = info[0];
  
  if (max_id >= 1) {
    cpuid (info, 1);
    cpu_features.sse42 = (info[2] & (1 << 20)) != 0;
    
    // Check OSXSAVE (bit 27) - required for AVX
    bool osxsave = (info[2] & (1 << 27)) != 0;
    
    if (osxsave && max_id >= 7) {
      cpuid (info, 7);
      cpu_features.avx2 = (info[1] & (1 << 5)) != 0;
      cpu_features.avx512f = (info[1] & (1 << 16)) != 0;
      cpu_features.gfni = (info[2] & (1 << 8)) != 0;
      cpu_features.avx512bw = (info[1] & (1 << 30)) != 0;
      cpu_features.avx512vl = (info[1] & (1 << 31)) != 0;
      cpu_features.avx512vbmi = (info[2] & (1 << 1)) != 0;
      cpu_features.avx512vpopcntdq = (info[2] & (1 << 14)) != 0;
      cpu_features.avx512bitalg = (info[2] & (1 << 12)) != 0;
    }
  }
#endif
  
  cpu_features.initialized = true;
  
  kissat_phase (solver, "simd", 0,
                "AVX-512F=%s AVX-512BW=%s AVX-512VL=%s AVX-512VPOPCNTDQ=%s "
                "AVX-512BITALG=%s GFNI=%s AVX2=%s SSE4.2=%s",
                cpu_features.avx512f ? "yes" : "no",
                cpu_features.avx512bw ? "yes" : "no",
                cpu_features.avx512vl ? "yes" : "no",
                cpu_features.avx512vpopcntdq ? "yes" : "no",
                cpu_features.avx512bitalg ? "yes" : "no",
                cpu_features.gfni ? "yes" : "no",
                cpu_features.avx2 ? "yes" : "no",
                cpu_features.sse42 ? "yes" : "no");
}

bool kissat_simd_available (kissat *solver) {
  (void) solver;
  if (!cpu_features.initialized)
    kissat_init_simd_support (solver);
  return cpu_features.avx512f && cpu_features.avx512bw;
}

/*
 * AVX-512 implementation of find_non_false
 * 
 * Strategy:
 * 1. Process literals in chunks of 16 (512 bits / 32 bits per literal)
 * 2. Use _mm512_i32gather_epi32 to gather values
 * 3. Use _mm512_cmpge_epi8_mask to find values >= 0
 * 4. Use _mm512_bitshuffle_epi64_mask (GFNI) or ctz to find first match
 */
#if KISSAT_HAS_AVX512
static inline bool avx512_find_non_false (const value *values,
                                           const unsigned *lits,
                                           size_t start_idx,
                                           size_t end_idx,
                                           unsigned *out_replacement,
                                           size_t *out_idx) {
  
  const size_t simd_width = 16; // 16 literals per AVX-512 register
  
  // Process 16 literals at a time
  size_t i = start_idx;
  
  // Align to 16-byte boundary if possible
  while (i < end_idx && ((uintptr_t)(lits + i) & 63) != 0) {
    unsigned lit = lits[i];
    if (values[lit] >= 0) {
      *out_replacement = lit;
      *out_idx = i;
      return true;
    }
    i++;
  }
  
  // Main SIMD loop - process 16 literals at once
  for (; i + simd_width <= end_idx; i += simd_width) {
    // Load 16 literal indices
    __m512i lit_indices = _mm512_loadu_si512 ((__m512i *)(lits + i));
    
    // Gather values for these literals
    // values[] is int8_t, we need to gather with scale 1
    __m512i lit_values = _mm512_i32gather_epi32 (
        lit_indices, values, 1);
    
    // Compare values >= 0 (signed comparison)
    // We want to find where value is NOT negative
    __mmask16 ge_mask = _mm512_cmpge_epi8_mask (lit_values, _mm512_setzero_si512 ());
    
    if (ge_mask != 0) {
      // Found at least one non-false literal
      // Get index of first match
      int first_match = __builtin_ctz (ge_mask);
      *out_replacement = lits[i + first_match];
      *out_idx = i + first_match;
      return true;
    }
  }
  
  // Handle remaining literals
  for (; i < end_idx; i++) {
    unsigned lit = lits[i];
    if (values[lit] >= 0) {
      *out_replacement = lit;
      *out_idx = i;
      return true;
    }
  }
  
  return false;
}
#endif // KISSAT_HAS_AVX512

/*
 * AVX2 implementation (256-bit)
 * Optimized for Intel and AMD Gen 4.5 (Zen 4)
 * 
 * Strategy for AVX2 (256-bit registers, 32 bytes):
 * - Process 32 literals at a time using byte-wise operations
 * - Load 32 literal indices (128 bytes) using 4x 256-bit loads
 * - Gather values[lit] for each - use _mm256_i32gather_epi32 with 32-bit zero extension
 * - Pack to bytes and compare
 * - AMD Zen 4 note: Has good AVX2 throughput, 256-bit ops are native (not split)
 * - Intel Alder Lake/Raptor Lake: Also good AVX2 performance
 */
#if KISSAT_HAS_AVX2

// Fast path: Process 32 literals at once using AVX2
// Returns number of literals processed (0-32), or 32+index+1 if found
// Returns 0 if no match in this batch
static inline unsigned avx2_find_in_batch_32 (const value *values,
                                               const unsigned *lits,
                                               unsigned *out_replacement) {
  // Load 32 literal indices (128 bytes = 4 AVX2 registers)
  // Each _mm256_loadu_si256 loads 8 x 32-bit integers
  __m256i lit0 = _mm256_loadu_si256 ((__m256i *)(lits + 0));
  __m256i lit1 = _mm256_loadu_si256 ((__m256i *)(lits + 8));
  __m256i lit2 = _mm256_loadu_si256 ((__m256i *)(lits + 16));
  __m256i lit3 = _mm256_loadu_si256 ((__m256i *)(lits + 24));
  
  // Gather 32-bit values and extract low byte
  // values[] is int8_t indexed by unsigned lit
  // We gather 32-bit and compare packed bytes
  
  // Note: _mm256_i32gather_epi32 with scale=1 gathers 32-bit values
  // We need the low byte of each gathered value
  
  // Gather first 8 values (32-bit each, we use low byte)
  __m256i v0 = _mm256_i32gather_epi32 ((const int *)values, lit0, 1);
  __m256i v1 = _mm256_i32gather_epi32 ((const int *)values, lit1, 1);
  __m256i v2 = _mm256_i32gather_epi32 ((const int *)values, lit2, 1);
  __m256i v3 = _mm256_i32gather_epi32 ((const int *)values, lit3, 1);
  
  // Pack from 32-bit to 8-bit: values are -1, 0, or 1
  // We want to check if value >= 0 (i.e., sign bit is 0)
  // First, pack 32-bit to 16-bit, then 16-bit to 8-bit
  
  // Pack 32-bit to 16-bit (signed saturation)
  __m256i v01_16 = _mm256_packs_epi32 (v0, v1);
  __m256i v23_16 = _mm256_packs_epi32 (v2, v3);
  
  // Pack 16-bit to 8-bit (signed saturation)
  __m256i v0123_8 = _mm256_packs_epi16 (v01_16, v23_16);
  
  // Now we have 32 x 8-bit values in v0123_8
  // Values are -1 (0xFF), 0, or 1 (0x01)
  // We want to find where value >= 0 (i.e., value != -1)
  
  // Create vector of -1
  __m256i neg1 = _mm256_set1_epi8 (-1);
  
  // Compare: v == -1 (false)
  __m256i false_mask = _mm256_cmpeq_epi8 (v0123_8, neg1);
  
  // We want non-false, so look for where false_mask is 0
  // Create mask of non-false positions
  int non_false_mask = _mm256_movemask_epi8 (false_mask);
  
  // Invert: we want positions where false_mask is 0
  int match_mask = ~non_false_mask & 0xFFFFFFFF;  // Keep only lower 32 bits
  
  if (match_mask != 0) {
    // Found at least one non-false
    int first = __builtin_ctz (match_mask);
    *out_replacement = lits[first];
    return 32 + first + 1;  // Encode found position
  }
  
  return 0;  // No match in this batch
}

// Alternative AVX2 implementation: 16-literal batches
// Better for clauses with 8-32 literals (most common case)
static inline bool avx2_find_non_false_16 (const value *values,
                                            const unsigned *lits,
                                            size_t start_idx,
                                            size_t end_idx,
                                            unsigned *out_replacement,
                                            size_t *out_idx) {
  
  const size_t simd_width = 16;  // 16 literals per batch
  size_t i = start_idx;
  
  // Align to 32-byte boundary if beneficial
  // But literals are 4 bytes, so 16 literals = 64 bytes
  // Just process sequentially
  
  for (; i + simd_width <= end_idx; i += simd_width) {
    // Load 16 literal indices (64 bytes)
    __m256i lit0 = _mm256_loadu_si256 ((__m256i *)(lits + i));
    __m256i lit1 = _mm256_loadu_si256 ((__m256i *)(lits + i + 8));
    
    // Gather values - each gather loads 8 x 32-bit values
    __m256i v0 = _mm256_i32gather_epi32 ((const int *)values, lit0, 1);
    __m256i v1 = _mm256_i32gather_epi32 ((const int *)values, lit1, 1);
    
    // Pack to 8-bit
    __m256i v01_16 = _mm256_packs_epi32 (v0, v1);
    __m256i v0123_8 = _mm256_packs_epi16 (v01_16, v01_16);  // Only need first 16
    
    // Extract lower 128 bits for the 16 values we care about
    __m128i v16 = _mm256_castsi256_si128 (v0123_8);
    
    // Compare with -1 (false)
    __m128i neg1 = _mm_set1_epi8 (-1);
    __m128i false_mask = _mm_cmpeq_epi8 (v16, neg1);
    
    // Get mask of non-false
    int non_false_mask = _mm_movemask_epi8 (false_mask);
    int match_mask = ~non_false_mask & 0xFFFF;
    
    if (match_mask != 0) {
      int first = __builtin_ctz (match_mask);
      *out_replacement = lits[i + first];
      *out_idx = i + first;
      return true;
    }
  }
  
  // Handle remaining with scalar
  for (; i < end_idx; i++) {
    unsigned lit = lits[i];
    if (values[lit] >= 0) {
      *out_replacement = lit;
      *out_idx = i;
      return true;
    }
  }
  
  return false;
}

// Optimized AVX2 implementation - safe version without gather
// Uses scalar loads to avoid potential gather issues
// Best for Intel and AMD Zen 4 (good at 256-bit operations)
static inline bool avx2_find_non_false (const value *values,
                                         const unsigned *lits,
                                         size_t start_idx,
                                         size_t end_idx,
                                         unsigned *out_replacement,
                                         size_t *out_idx) {
  
  size_t i = start_idx;
  const size_t count = end_idx - start_idx;
  
  // For small counts, use scalar (avoid SIMD overhead)
  if (count < 8) {
    for (; i < end_idx; i++) {
      unsigned lit = lits[i];
      if (values[lit] >= 0) {
        *out_replacement = lit;
        *out_idx = i;
        return true;
      }
    }
    return false;
  }
  
  // Main AVX2 loop: process up to 8 literals at a time
  // We use scalar loads to build the vector - safer than gather
  for (; i + 8 <= end_idx; i += 8) {
    // Load values directly using scalar accesses - the compiler will optimize
    // This avoids gather which can have issues with unaligned/short arrays
    int8_t v0 = values[lits[i + 0]];
    int8_t v1 = values[lits[i + 1]];
    int8_t v2 = values[lits[i + 2]];
    int8_t v3 = values[lits[i + 3]];
    int8_t v4 = values[lits[i + 4]];
    int8_t v5 = values[lits[i + 5]];
    int8_t v6 = values[lits[i + 6]];
    int8_t v7 = values[lits[i + 7]];
    
    // Build vector from scalar values (32 bytes total for AVX2)
    // We only use the lower 8 bytes, upper 24 bytes are padding
    __m256i bytes = _mm256_set_epi8(
        0, 0, 0, 0, 0, 0, 0, 0,  // Bytes 31-24
        0, 0, 0, 0, 0, 0, 0, 0,  // Bytes 23-16
        0, 0, 0, 0, 0, 0, 0, 0,  // Bytes 15-8
        v7, v6, v5, v4, v3, v2, v1, v0  // Bytes 7-0 with values
    );
    
    // Values are -1, 0, or 1
    // Non-false means NOT (value == -1)
    __m256i neg1_vec = _mm256_set1_epi8 (-1);
    __m256i is_false = _mm256_cmpeq_epi8 (bytes, neg1_vec);
    int false_mask = _mm256_movemask_epi8 (is_false);
    
    // We want positions where it's NOT false (only check lower 8 bits)
    int match_mask = (~false_mask) & 0xFF;
    
    if (match_mask != 0) {
      int first = __builtin_ctz (match_mask);
      *out_replacement = lits[i + first];
      *out_idx = i + first;
      return true;
    }
  }
  
  // Handle remaining (0-7 literals) with scalar
  for (; i < end_idx; i++) {
    unsigned lit = lits[i];
    if (values[lit] >= 0) {
      *out_replacement = lit;
      *out_idx = i;
      return true;
    }
  }
  
  return false;
}

// Even faster AVX2: 4x unrolled for maximum ILP
// Safe version without gather - uses scalar loads
static inline bool avx2_find_non_false_unrolled (const value *values,
                                                  const unsigned *lits,
                                                  size_t start_idx,
                                                  size_t end_idx,
                                                  unsigned *out_replacement,
                                                  size_t *out_idx) {
  
  size_t i = start_idx;
  
  // Process 32 literals at a time using simple scalar loop unrolled
  // This avoids gather issues while still getting good ILP
  for (; i + 32 <= end_idx; i += 32) {
    // Check first 8
    for (int j = 0; j < 8; j++) {
      if (values[lits[i + j]] >= 0) {
        *out_replacement = lits[i + j];
        *out_idx = i + j;
        return true;
      }
    }
    // Check next 8
    for (int j = 8; j < 16; j++) {
      if (values[lits[i + j]] >= 0) {
        *out_replacement = lits[i + j];
        *out_idx = i + j;
        return true;
      }
    }
    // Check next 8
    for (int j = 16; j < 24; j++) {
      if (values[lits[i + j]] >= 0) {
        *out_replacement = lits[i + j];
        *out_idx = i + j;
        return true;
      }
    }
    // Check last 8
    for (int j = 24; j < 32; j++) {
      if (values[lits[i + j]] >= 0) {
        *out_replacement = lits[i + j];
        *out_idx = i + j;
        return true;
      }
    }
  }
  
  // Handle remaining with simpler AVX2
  return avx2_find_non_false (values, lits, i, end_idx, out_replacement, out_idx);
}
#endif // KISSAT_HAS_AVX2

// Main dispatch function
bool kissat_simd_find_non_false (const value *values,
                                  const unsigned *lits,
                                  size_t start_idx,
                                  size_t end_idx,
                                  unsigned *out_replacement,
                                  size_t *out_idx) {
  
  size_t count = end_idx - start_idx;
  
  // Use scalar for small arrays (avoid SIMD overhead)
  if (count < KISSAT_SIMD_THRESHOLD) {
    return scalar_find_non_false (values, lits, start_idx, end_idx,
                                   out_replacement, out_idx);
  }
  
#if KISSAT_HAS_AVX512
  if (cpu_features.avx512f && cpu_features.avx512bw) {
    return avx512_find_non_false (values, lits, start_idx, end_idx,
                                   out_replacement, out_idx);
  }
#endif
  
#if KISSAT_HAS_AVX2
  if (cpu_features.avx2) {
    // Use unrolled version for large clauses (>32 literals)
    // This maximizes ILP on both Intel and AMD Zen 4
    if (count >= 32) {
      return avx2_find_non_false_unrolled (values, lits, start_idx, end_idx,
                                           out_replacement, out_idx);
    }
    // Use standard version for medium clauses (8-31 literals)
    return avx2_find_non_false (values, lits, start_idx, end_idx,
                                 out_replacement, out_idx);
  }
#endif
  
  // Fallback to scalar
  return scalar_find_non_false (values, lits, start_idx, end_idx,
                                 out_replacement, out_idx);
}

// Count falsified literals using SIMD
size_t kissat_simd_count_false (const value *values,
                                 const unsigned *lits,
                                 size_t size) {
  
  if (size < KISSAT_SIMD_THRESHOLD) {
    size_t count = 0;
    for (size_t i = 0; i < size; i++) {
      if (values[lits[i]] < 0)
        count++;
    }
    return count;
  }
  
#if KISSAT_HAS_AVX512
  if (cpu_features.avx512f && cpu_features.avx512bw) {
    size_t count = 0;
    const size_t simd_width = 16;
    size_t i = 0;
    
    for (; i + simd_width <= size; i += simd_width) {
      __m512i lit_indices = _mm512_loadu_si512 ((__m512i *)(lits + i));
      __m512i lit_values = _mm512_i32gather_epi32 (lit_indices, values, 1);
      
      // Count negative values (sign bit set)
      __mmask16 neg_mask = _mm512_movepi8_mask (lit_values);
      count += __builtin_popcount (neg_mask);
    }
    
    // Handle remainder
    for (; i < size; i++) {
      if (values[lits[i]] < 0)
        count++;
    }
    return count;
  }
#endif
  
  // AVX2 implementation
#if KISSAT_HAS_AVX2
  if (cpu_features.avx2 && size >= 16) {
    size_t count = 0;
    size_t i = 0;
    
    // Use 2x unrolled loop for ILP
    for (; i + 16 <= size; i += 16) {
      __m256i lit0 = _mm256_loadu_si256 ((__m256i *)(lits + i));
      __m256i lit1 = _mm256_loadu_si256 ((__m256i *)(lits + i + 8));
      
      __m256i v0 = _mm256_i32gather_epi32 ((const int *)values, lit0, 1);
      __m256i v1 = _mm256_i32gather_epi32 ((const int *)values, lit1, 1);
      
      // Check sign bit (value < 0 means negative)
      __m256i neg0 = _mm256_cmpgt_epi8 (_mm256_setzero_si256 (), 
                                         _mm256_and_si256 (v0, _mm256_set1_epi32 (0xFF)));
      __m256i neg1 = _mm256_cmpgt_epi8 (_mm256_setzero_si256 (), 
                                         _mm256_and_si256 (v1, _mm256_set1_epi32 (0xFF)));
      
      int mask0 = _mm256_movemask_epi8 (neg0);
      int mask1 = _mm256_movemask_epi8 (neg1);
      
      count += (unsigned)__builtin_popcount (mask0 & 0xFF);
      count += (unsigned)__builtin_popcount (mask1 & 0xFF);
    }
    
    // Handle remaining 8
    for (; i + 8 <= size; i += 8) {
      __m256i lit_vec = _mm256_loadu_si256 ((__m256i *)(lits + i));
      __m256i val_vec = _mm256_i32gather_epi32 ((const int *)values, lit_vec, 1);
      __m256i neg_mask = _mm256_cmpgt_epi8 (_mm256_setzero_si256 (),
                                             _mm256_and_si256 (val_vec, _mm256_set1_epi32 (0xFF)));
      int mask = _mm256_movemask_epi8 (neg_mask);
      count += __builtin_popcount (mask & 0xFF);
    }
    
    // Handle remainder
    for (; i < size; i++) {
      if (values[lits[i]] < 0)
        count++;
    }
    return count;
  }
#endif
  
  // Scalar fallback
  size_t count = 0;
  for (size_t i = 0; i < size; i++) {
    if (values[lits[i]] < 0)
      count++;
  }
  return count;
}

// Check if all literals are false
bool kissat_simd_all_false (const value *values,
                             const unsigned *lits,
                             size_t size) {
  
#if KISSAT_HAS_AVX512
  if (size >= 16 && cpu_features.avx512f && cpu_features.avx512bw) {
    const size_t simd_width = 16;
    size_t i = 0;
    
    for (; i + simd_width <= size; i += simd_width) {
      __m512i lit_indices = _mm512_loadu_si512 ((__m512i *)(lits + i));
      __m512i lit_values = _mm512_i32gather_epi32 (lit_indices, values, 1);
      
      // Check if ANY value is >= 0 (not false)
      __mmask16 ge_mask = _mm512_cmpge_epi8_mask (lit_values, _mm512_setzero_si512 ());
      if (ge_mask != 0) {
        return false;
      }
    }
    
    // Check remainder
    for (; i < size; i++) {
      if (values[lits[i]] >= 0)
        return false;
    }
    return true;
  }
#endif
  
  // AVX2 implementation
#if KISSAT_HAS_AVX2
  if (size >= 16 && cpu_features.avx2) {
    size_t i = 0;
    
    // 2x unrolled for ILP
    for (; i + 16 <= size; i += 16) {
      __m256i lit0 = _mm256_loadu_si256 ((__m256i *)(lits + i));
      __m256i lit1 = _mm256_loadu_si256 ((__m256i *)(lits + i + 8));
      
      __m256i v0 = _mm256_i32gather_epi32 ((const int *)values, lit0, 1);
      __m256i v1 = _mm256_i32gather_epi32 ((const int *)values, lit1, 1);
      
      // Check if any value is >= 0 (sign bit is 0)
      // Pack to bytes first
      __m256i bytes0 = _mm256_and_si256 (v0, _mm256_set1_epi32 (0xFF));
      __m256i bytes1 = _mm256_and_si256 (v1, _mm256_set1_epi32 (0xFF));
      
      // Check >= 0: compare with zero (greater than -1)
      __m256i ge0 = _mm256_cmpgt_epi8 (bytes0, _mm256_set1_epi8 (-1));
      __m256i ge1 = _mm256_cmpgt_epi8 (bytes1, _mm256_set1_epi8 (-1));
      
      int mask0 = _mm256_movemask_epi8 (ge0);
      int mask1 = _mm256_movemask_epi8 (ge1);
      
      if ((mask0 & 0xFF) != 0 || (mask1 & 0xFF) != 0) {
        return false;  // Found a non-false literal
      }
    }
    
    // Remaining 8
    for (; i + 8 <= size; i += 8) {
      __m256i lit_vec = _mm256_loadu_si256 ((__m256i *)(lits + i));
      __m256i val_vec = _mm256_i32gather_epi32 ((const int *)values, lit_vec, 1);
      __m256i bytes = _mm256_and_si256 (val_vec, _mm256_set1_epi32 (0xFF));
      __m256i ge = _mm256_cmpgt_epi8 (bytes, _mm256_set1_epi8 (-1));
      
      if (_mm256_movemask_epi8 (ge) & 0xFF) {
        return false;
      }
    }
    
    // Check remainder
    for (; i < size; i++) {
      if (values[lits[i]] >= 0)
        return false;
    }
    return true;
  }
#endif
  
  // Scalar fallback
  for (size_t i = 0; i < size; i++) {
    if (values[lits[i]] >= 0)
      return false;
  }
  return true;
}

// ============================================================
// LITERAL MEMBERSHIP TESTING
// Find if literal_idx exists in lits array
// ============================================================

#if KISSAT_HAS_AVX512
static inline size_t avx512_find_literal_idx (unsigned lit_idx,
                                               const unsigned *lits,
                                               size_t size) {
  
  // Broadcast the target literal index to all lanes
  __m512i target = _mm512_set1_epi32 (lit_idx);
  const size_t simd_width = 16;
  size_t i = 0;
  
  for (; i + simd_width <= size; i += simd_width) {
    // Load 16 literal indices
    __m512i candidates = _mm512_loadu_si512 ((__m512i *)(lits + i));
    
    // Compare with target - get mask of matches
    __mmask16 match_mask = _mm512_cmpeq_epi32_mask (candidates, target);
    
    if (match_mask != 0) {
      // Found it - return the index
      return i + __builtin_ctz (match_mask);
    }
  }
  
  // Handle remainder
  for (; i < size; i++) {
    if (lits[i] == lit_idx)
      return i;
  }
  
  return size; // Not found
}
#endif // KISSAT_HAS_AVX512

#if KISSAT_HAS_AVX2
static inline size_t avx2_find_literal_idx (unsigned lit_idx,
                                             const unsigned *lits,
                                             size_t size) {
  
  __m256i target = _mm256_set1_epi32 (lit_idx);
  const size_t simd_width = 8;
  size_t i = 0;
  
  for (; i + simd_width <= size; i += simd_width) {
    __m256i candidates = _mm256_loadu_si256 ((__m256i *)(lits + i));
    __m256i cmp = _mm256_cmpeq_epi32 (candidates, target);
    int mask = _mm256_movemask_epi8 (cmp);
    
    if (mask != 0) {
      // Each 32-bit compare produces 4 bytes in mask
      // Find first set bit and divide by 4
      return i + (__builtin_ctz (mask) / 4);
    }
  }
  
  // Handle remainder
  for (; i < size; i++) {
    if (lits[i] == lit_idx)
      return i;
  }
  
  return size;
}
#endif // KISSAT_HAS_AVX2

size_t kissat_simd_find_literal_idx (unsigned lit_idx,
                                      const unsigned *lits,
                                      size_t size) {
  
  if (size < 4) {
    // Scalar for very small arrays
    for (size_t i = 0; i < size; i++) {
      if (lits[i] == lit_idx)
        return i;
    }
    return size;
  }
  
#if KISSAT_HAS_AVX512
  if (cpu_features.avx512f) {
    return avx512_find_literal_idx (lit_idx, lits, size);
  }
#endif
  
#if KISSAT_HAS_AVX2
  if (cpu_features.avx2) {
    return avx2_find_literal_idx (lit_idx, lits, size);
  }
#endif
  
  // Scalar fallback
  for (size_t i = 0; i < size; i++) {
    if (lits[i] == lit_idx)
      return i;
  }
  return size;
}

// ============================================================
// BATCH MARKING OF LITERALS
// Set marks[lits[i]] = mark_value for multiple literals
// ============================================================

void kissat_simd_mark_literals (value *marks,
                                 const unsigned *lits,
                                 size_t size,
                                 value mark_value) {
  
  // This is essentially a scatter operation
  // AVX-512 has _mm512_i32scatter_epi32 but it's slow on many CPUs
  // Better to use simple loop with potential unrolling
  
#if KISSAT_HAS_AVX512
  if (size >= 16 && cpu_features.avx512f) {
    const size_t simd_width = 16;
    size_t i = 0;
    
    // Process 16 indices at once with software prefetching
    for (; i + simd_width <= size; i += simd_width) {
      // Prefetch next batch of literals
      _mm_prefetch ((const char *)(lits + i + simd_width), _MM_HINT_T0);
      
      // Actually set the marks (scatter is slow, do scalar stores)
      for (size_t j = 0; j < simd_width; j++) {
        marks[lits[i + j]] = mark_value;
      }
    }
    
    // Handle remainder
    for (; i < size; i++) {
      marks[lits[i]] = mark_value;
    }
    return;
  }
#endif
  
  // Scalar with simple unrolling for ILP
  size_t i = 0;
  for (; i + 4 <= size; i += 4) {
    marks[lits[i + 0]] = mark_value;
    marks[lits[i + 1]] = mark_value;
    marks[lits[i + 2]] = mark_value;
    marks[lits[i + 3]] = mark_value;
  }
  for (; i < size; i++) {
    marks[lits[i]] = mark_value;
  }
}

// ============================================================
// CONFLICT CLAUSE ANALYSIS
// Combined operation for conflict analysis
// ============================================================

bool kissat_simd_analyze_conflict_literals (kissat *solver,
                                             const unsigned *lits,
                                             size_t size,
                                             unsigned not_failed,
                                             unsigned failed,
                                             unsigned *out_analyzed_count) {
  
  (void) failed; // unused but kept for interface
  
  assigned *all_assigned = solver->assigned;
  unsigned count = 0;
  
#if KISSAT_HAS_AVX512
  if (size >= 16 && cpu_features.avx512f && cpu_features.avx512bw) {
    const size_t simd_width = 16;
    size_t i = 0;
    
    // Broadcast not_failed for comparison
    __m512i not_failed_vec = _mm512_set1_epi32 (not_failed);
    
    for (; i + simd_width <= size; i += simd_width) {
      __m512i lit_vec = _mm512_loadu_si512 ((__m512i *)(lits + i));
      
      // Check if any literal equals not_failed
      __mmask16 is_not_failed = _mm512_cmpeq_epi32_mask (lit_vec, not_failed_vec);
      if (is_not_failed != 0) {
        return true; // Special case - contains negation of failed
      }
      
      // Process each literal
      // We can't easily parallelize the level/analyzed checks
      // due to memory dependencies, so do scalar for logic
      for (size_t j = 0; j < simd_width; j++) {
        unsigned lit = lits[i + j];
        const unsigned idx = IDX (lit);
        assigned *a = all_assigned + idx;
        
        if (!a->level)
          continue;
        
        if (!a->analyzed) {
          kissat_push_analyzed (solver, all_assigned, idx);
          count++;
        }
      }
    }
    
    // Handle remainder
    for (; i < size; i++) {
      unsigned lit = lits[i];
      if (lit == not_failed)
        return true;
      
      const unsigned idx = IDX (lit);
      assigned *a = all_assigned + idx;
      
      if (!a->level)
        continue;
      
      if (!a->analyzed) {
        kissat_push_analyzed (solver, all_assigned, idx);
        count++;
      }
    }
    
    *out_analyzed_count = count;
    return false;
  }
#endif
  
  // Scalar fallback
  for (size_t i = 0; i < size; i++) {
    unsigned lit = lits[i];
    if (lit == not_failed)
      return true;
    
    const unsigned idx = IDX (lit);
    assigned *a = all_assigned + idx;
    
    if (!a->level)
      continue;
    
    if (!a->analyzed) {
      kissat_push_analyzed (solver, all_assigned, idx);
      count++;
    }
  }
  
  *out_analyzed_count = count;
  return false;
}
