#ifndef _simdscan_h_INCLUDED
#define _simdscan_h_INCLUDED

#include "internal.h"
#include "value.h"

/*
 * SIMD-optimized clause literal scanning
 * 
 * Uses AVX-512 with GFNI, BITALG, and VPOPCNTDQ when available
 * Falls back to SSE4.2/AVX2 or scalar code otherwise
 * 
 * Goal: Find first literal in clause where values[literal] >= 0
 */

// Detect AVX-512 support at compile time
#if defined(__AVX512F__) && defined(__AVX512VL__) && defined(__AVX512BW__)
#define KISSAT_HAS_AVX512 1
#else
#define KISSAT_HAS_AVX512 0
#endif

#if defined(__AVX512VPOPCNTDQ__) && defined(__AVX512BITALG__)
#define KISSAT_HAS_AVX512_BITOPS 1
#else
#define KISSAT_HAS_AVX512_BITOPS 0
#endif

#if defined(__GFNI__) && defined(__AVX512BW__)
#define KISSAT_HAS_GFNI 1
#else
#define KISSAT_HAS_GFNI 0
#endif

#if defined(__AVX2__)
#define KISSAT_HAS_AVX2 1
#else
#define KISSAT_HAS_AVX2 0
#endif

#if defined(__SSE4_2__)
#define KISSAT_HAS_SSE42 1
#else
#define KISSAT_HAS_SSE42 0
#endif

#include <immintrin.h>

// Initialize SIMD support detection
void kissat_init_simd_support (kissat *solver);

// Returns true if SIMD is available and beneficial
bool kissat_simd_available (kissat *solver);

/*
 * Scan for first non-false literal in clause using SIMD
 * 
 * @param values - the values array (values[literal] gives -1, 0, or 1)
 * @param lits - array of literals to scan
 * @param start_idx - starting index in lits array
 * @param end_idx - ending index (exclusive)
 * @param out_replacement - output: the found literal
 * @param out_idx - output: index of found literal
 * @return true if found, false otherwise
 */
bool kissat_simd_find_non_false (const value *values,
                                  const unsigned *lits,
                                  size_t start_idx,
                                  size_t end_idx,
                                  unsigned *out_replacement,
                                  size_t *out_idx);

/*
 * Count falsified literals in a clause using SIMD
 * Useful for quickly checking if clause is unit/conflict
 * 
 * @param values - the values array
 * @param lits - array of literals
 * @param size - number of literals
 * @return count of literals where values[lit] < 0
 */
size_t kissat_simd_count_false (const value *values,
                                 const unsigned *lits,
                                 size_t size);

/*
 * Check if all literals in a range are false (for subsumption checking)
 * Uses AVX-512 VPTEST for parallel comparison
 * 
 * @param values - the values array  
 * @param lits - array of literals
 * @param size - number of literals
 * @return true if all values[lits[i]] < 0
 */
bool kissat_simd_all_false (const value *values,
                             const unsigned *lits,
                             size_t size);

// Inline helpers for scalar fallback
static inline bool scalar_find_non_false (const value *values,
                                           const unsigned *lits,
                                           size_t start_idx,
                                           size_t end_idx,
                                           unsigned *out_replacement,
                                           size_t *out_idx) {
  for (size_t i = start_idx; i < end_idx; i++) {
    unsigned lit = lits[i];
    if (values[lit] >= 0) {
      *out_replacement = lit;
      *out_idx = i;
      return true;
    }
  }
  return false;
}

// Threshold for using SIMD (must be large enough to amortize setup cost)
#define KISSAT_SIMD_THRESHOLD 8

#endif // _simdscan_h_INCLUDED
