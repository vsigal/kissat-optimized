#ifndef _simdconfig_h_INCLUDED
#define _simdconfig_h_INCLUDED

// Enhanced SIMD configuration for Kissat
// Detects and enables AVX-512, GFNI, and other advanced instructions

#if defined(__x86_64__) || defined(_M_X64)
  // x86-64 platform
  
  // AVX-512 detection
  #if defined(__AVX512F__) && defined(__AVX512BW__) && defined(__AVX512VL__)
    #define KISSAT_HAS_AVX512 1
    
    // AVX-512 VPOPCNTDQ - population count for 64-bit integers
    #ifdef __AVX512VPOPCNTDQ__
      #define KISSAT_HAS_AVX512_VPOPCNT 1
    #endif
    
    // AVX-512 BITALG - bit manipulation
    #ifdef __AVX512BITALG__
      #define KISSAT_HAS_AVX512_BITALG 1
    #endif
    
    // AVX-512 VBMI/VBMI2 - byte manipulation
    #ifdef __AVX512VBMI__
      #define KISSAT_HAS_AVX512_VBMI 1
    #endif
    #ifdef __AVX512VBMI2__
      #define KISSAT_HAS_AVX512_VBMI2 1
    #endif
    
    // AVX-512 VP2INTERSECT - intersection operations
    #ifdef __AVX512VP2INTERSECT__
      #define KISSAT_HAS_AVX512_VP2INTERSECT 1
    #endif
    
    // AVX-512 VNNI - neural network instructions (useful for packed comparisons)
    #ifdef __AVX512VNNI__
      #define KISSAT_HAS_AVX512_VNNI 1
    #endif
  #endif
  
  // GFNI detection
  #ifdef __GFNI__
    #define KISSAT_HAS_GFNI 1
  #endif
  
  // VAES/VPCLMULQDQ for cryptographic operations (sometimes useful for bit manipulation)
  #ifdef __VAES__
    #define KISSAT_HAS_VAES 1
  #endif
  #ifdef __VPCLMULQDQ__
    #define KISSAT_HAS_VPCLMULQDQ 1
  #endif
  
  // AVX2 baseline
  #ifdef __AVX2__
    #define KISSAT_HAS_AVX2 1
  #endif
  
  // SSE4.2 baseline  
  #ifdef __SSE4_2__
    #define KISSAT_HAS_SSE42 1
  #endif
  
  // POPCNT instruction
  #ifdef __POPCNT__
    #define KISSAT_HAS_POPCNT 1
  #endif
  
  // LZCNT/TZCNT (leading/trailing zero count)
  #ifdef __LZCNT__
    #define KISSAT_HAS_LZCNT 1
  #endif
  #ifdef __BMI__
    #define KISSAT_HAS_TZCNT 1  // BMI provides TZCNT
  #endif

#elif defined(__aarch64__) || defined(_M_ARM64)
  // ARM64 platform
  
  // NEON is standard on AArch64
  #define KISSAT_HAS_NEON 1
  
  // SVE detection (Scalable Vector Extensions)
  #ifdef __ARM_FEATURE_SVE
    #define KISSAT_HAS_SVE 1
  #endif
  
  // SVE2 detection
  #ifdef __ARM_FEATURE_SVE2
    #define KISSAT_HAS_SVE2 1
  #endif

#endif // Platform detection

// Feature validation and fallbacks
#ifndef KISSAT_HAS_AVX512
  #define KISSAT_HAS_AVX512 0
#endif
#ifndef KISSAT_HAS_AVX512_VPOPCNT
  #define KISSAT_HAS_AVX512_VPOPCNT 0
#endif
#ifndef KISSAT_HAS_AVX512_BITALG
  #define KISSAT_HAS_AVX512_BITALG 0
#endif
#ifndef KISSAT_HAS_AVX512_VBMI
  #define KISSAT_HAS_AVX512_VBMI 0
#endif
#ifndef KISSAT_HAS_AVX512_VBMI2
  #define KISSAT_HAS_AVX512_VBMI2 0
#endif
#ifndef KISSAT_HAS_AVX512_VP2INTERSECT
  #define KISSAT_HAS_AVX512_VP2INTERSECT 0
#endif
#ifndef KISSAT_HAS_AVX512_VNNI
  #define KISSAT_HAS_AVX512_VNNI 0
#endif
#ifndef KISSAT_HAS_GFNI
  #define KISSAT_HAS_GFNI 0
#endif
#ifndef KISSAT_HAS_VAES
  #define KISSAT_HAS_VAES 0
#endif
#ifndef KISSAT_HAS_VPCLMULQDQ
  #define KISSAT_HAS_VPCLMULQDQ 0
#endif
#ifndef KISSAT_HAS_AVX2
  #define KISSAT_HAS_AVX2 0
#endif
#ifndef KISSAT_HAS_SSE42
  #define KISSAT_HAS_SSE42 0
#endif
#ifndef KISSAT_HAS_POPCNT
  #define KISSAT_HAS_POPCNT 0
#endif
#ifndef KISSAT_HAS_LZCNT
  #define KISSAT_HAS_LZCNT 0
#endif
#ifndef KISSAT_HAS_TZCNT
  #define KISSAT_HAS_TZCNT 0
#endif
#ifndef KISSAT_HAS_NEON
  #define KISSAT_HAS_NEON 0
#endif
#ifndef KISSAT_HAS_SVE
  #define KISSAT_HAS_SVE 0
#endif
#ifndef KISSAT_HAS_SVE2
  #define KISSAT_HAS_SVE2 0
#endif

#endif // _simdconfig_h_INCLUDED
