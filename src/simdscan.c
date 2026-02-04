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
 * Processes 8 literals at a time
 */
#if KISSAT_HAS_AVX2
static inline bool avx2_find_non_false (const value *values,
                                         const unsigned *lits,
                                         size_t start_idx,
                                         size_t end_idx,
                                         unsigned *out_replacement,
                                         size_t *out_idx) {
  
  const size_t simd_width = 8; // 8 literals per AVX2 register
  size_t i = start_idx;
  
  // Process 8 literals at a time
  for (; i + simd_width <= end_idx; i += simd_width) {
    // Load 8 literal indices
    __m256i lit_indices = _mm256_loadu_si256 ((__m256i *)(lits + i));
    
    // Gather values - AVX2 has _mm256_i32gather_epi32 but it loads 32-bit values
    // Our values are 8-bit, so we need to be careful
    // For now, use scalar fallback within AVX2 loop
    // (AVX2 doesn't have 8-bit gather)
    
    // Actually, let's just do 8 scalar checks but unrolled
    // which helps with instruction-level parallelism
    value v0 = values[lits[i + 0]];
    value v1 = values[lits[i + 1]];
    value v2 = values[lits[i + 2]];
    value v3 = values[lits[i + 3]];
    value v4 = values[lits[i + 4]];
    value v5 = values[lits[i + 5]];
    value v6 = values[lits[i + 6]];
    value v7 = values[lits[i + 7]];
    
    // Branchless check using bit operations
    unsigned mask = 0;
    mask |= (v0 >= 0) << 0;
    mask |= (v1 >= 0) << 1;
    mask |= (v2 >= 0) << 2;
    mask |= (v3 >= 0) << 3;
    mask |= (v4 >= 0) << 4;
    mask |= (v5 >= 0) << 5;
    mask |= (v6 >= 0) << 6;
    mask |= (v7 >= 0) << 7;
    
    if (mask != 0) {
      int first = __builtin_ctz (mask);
      *out_replacement = lits[i + first];
      *out_idx = i + first;
      return true;
    }
  }
  
  // Handle remaining
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
#endif // KISSAT_HAS_AVX2

// Main dispatch function
bool kissat_simd_find_non_false (const value *values,
                                  const unsigned *lits,
                                  size_t start_idx,
                                  size_t end_idx,
                                  unsigned *out_replacement,
                                  size_t *out_idx) {
  
  size_t count = end_idx - start_idx;
  
  // Use scalar for small arrays
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
  
  // AVX2 fallback
#if KISSAT_HAS_AVX2
  if (cpu_features.avx2) {
    size_t count = 0;
    // Similar to above but with 256-bit registers
    for (size_t i = 0; i < size; i++) {
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
    
    // Process 16 indices at once, but scatter individually
    // (scatters are slow, so we just use this for prefetching)
    for (; i + simd_width <= size; i += simd_width) {
      __m512i indices = _mm512_loadu_si512 ((__m512i *)(lits + i));
      // Prefetch mark locations
      _mm512_prefetch_i32scatter_ps (marks, indices, 1, _MM_HINT_T0);
      
      // Actually set the marks
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
  value *marks = solver->marks;
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
