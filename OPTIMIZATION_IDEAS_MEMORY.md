# 5 Ideas to Address Memory Bottleneck

Based on f2.cnf analysis showing backend bound at 27.5% and cache miss rates of 6.10% (L1) and 3.27% (LLC).

---

## Idea #1: Structure-of-Arrays (SoA) for Clause Data

**Problem**: Current clause structure mixes hot/cold data causing cache pollution

**Current Layout** (Array-of-Structures):
```c
struct clause {
  unsigned size;      // Hot (frequently accessed)
  bool garbage;       // Cold (rarely accessed)
  bool redundant;     // Cold
  unsigned used;      // Medium
  unsigned searched;  // Hot
  unsigned lits[];    // Hot
};
```

**Proposed Layout** (Structure-of-Arrays):
```c
struct clause_pool {
  // Hot data - separate array
  unsigned *sizes;        // Contiguous
  unsigned *searched;     // Contiguous
  unsigned **lits;        // Pointers to literal arrays
  
  // Cold data - separate array (different cache line)
  bool *garbage;
  bool *redundant;
  unsigned *used;
};
```

**Expected Benefit**: 
- 15-20% reduction in L1 misses
- Better spatial locality during propagation
- ~8-12% speedup

**Implementation Complexity**: High (touches many files)

---

## Idea #2: Software Prefetching for Watch Lists

**Problem**: Watch list traversal has irregular access patterns, cache misses stall pipeline

**Current Code**:
```c
while (p != end_watches) {
  const watch head = *q++ = *p++;  // Cache miss here
  const unsigned blocking = head.blocking.lit;
  const value blocking_value = values[blocking];  // Cache miss here
  // ...
}
```

**Proposed Enhancement**:
```c
#define PREFETCH_DISTANCE 16

while (p != end_watches) {
  // Prefetch multiple watches ahead
  if (p + PREFETCH_DISTANCE < end_watches)
    __builtin_prefetch(p + PREFETCH_DISTANCE, 0, 0);
  
  const watch head = *q++ = *p++;
  const unsigned blocking = head.blocking.lit;
  
  // Prefetch blocking literal's value
  __builtin_prefetch(&values[blocking], 0, 0);
  
  const value blocking_value = values[blocking];
  // ...
}
```

**Expected Benefit**:
- 20-30% reduction in L1 misses
- Hide memory latency via pipelining
- ~5-10% speedup

**Implementation Complexity**: Low (localized changes)

---

## Idea #3: Compact Clause Representation

**Problem**: Clause overhead is high (32 bytes header + literals). Small clauses waste cache.

**Current Size**:
- Binary clause: 32 bytes header + 8 bytes literals = 40 bytes
- Ternary clause: 32 bytes + 12 bytes = 44 bytes

**Proposed Packed Format**:
```c
// Special encoding for small clauses
// Use high bits of reference to indicate type

#define BINARY_TAG 0x80000000  // Binary clause marker
#define TERNARY_TAG 0x40000000 // Ternary clause marker

// Binary clause: packed into 64 bits
// [1 bit: binary flag][31 bits: lit1][32 bits: lit2]

// Store directly in watch, no arena allocation needed!
```

**Impact**:
- Binary/ternary clauses: 40-44 bytes → 8 bytes (80% reduction)
- Eliminates arena allocation for 70% of clauses
- Massive cache footprint reduction

**Expected Benefit**:
- 40-50% reduction in memory traffic
- 15-25% speedup
- Reduced arena fragmentation

**Implementation Complexity**: Medium (requires watch format change)

---

## Idea #4: Hierarchical Watch Lists by Activity

**Problem**: All watches mixed together - hot (recently learned) and cold (old) clauses compete for cache.

**Current**: Single watch list per literal

**Proposed**: Tiered watch lists
```c
typedef struct tiered_watches {
  // Tier 1: Recently learned, high activity (hot)
  // Small, fits in L1 cache
  watch hot[MAX_HOT_WATCHES];  // ~64 entries
  
  // Tier 2: Medium activity  
  vector warm;  // Standard vector
  
  // Tier 3: Low activity (cold)
  vector cold;  // Process less frequently
} tiered_watches;
```

**Algorithm**:
1. Always check "hot" first (cache resident)
2. Promote/demote based on activity/usage
3. Process "cold" less frequently (every N propagations)

**Expected Benefit**:
- Hot working set fits in L1 cache
- 30-40% of propagations use only hot list
- ~10-15% speedup

**Implementation Complexity**: Medium (changes watch structure and propagation)

---

## Idea #5: NUMA-Aware Memory Allocation

**Problem**: On multi-socket systems, memory access crosses NUMA nodes causing high latency

**Current**: All memory allocated on first-touch NUMA node

**Proposed**: Interleaved allocation for large structures
```c
// Allocate arena across all NUMA nodes
#ifdef NUMA_SUPPORT
#include <numa.h>

void *numa_allocate_interleaved(size_t size) {
  return numa_alloc_interleaved(size);
}

// Use for:
// - solver->arena (clause storage)
// - solver->watches (watch lists)
// - solver->values (assignment array)
#endif
```

**Additional Optimizations**:
1. Pin solver thread to specific core
2. Allocate scratch buffers locally
3. Minimize cross-node memory traffic

**Expected Benefit** (on dual-socket systems):
- 20-30% reduction in memory latency
- 10-20% speedup on large problems
- Better scaling with core count

**Implementation Complexity**: Low-Medium (conditional compilation)

---

## Summary Table

| Idea | Speedup | Complexity | Risk |
|------|---------|------------|------|
| #1 Structure-of-Arrays | 8-12% | High | Medium |
| #2 Enhanced Prefetching | 5-10% | Low | Low |
| #3 Compact Clauses | 15-25% | Medium | Medium |
| #4 Hierarchical Watches | 10-15% | Medium | Low |
| #5 NUMA Awareness | 10-20% | Low-Med | Low |

**Recommended Order**:
1. Start with #2 (quick win, validates approach)
2. Then #3 (biggest impact, medium effort)
3. Then #4 (builds on #3)
4. #5 if running on multi-socket hardware
5. #1 for long-term architectural improvement
