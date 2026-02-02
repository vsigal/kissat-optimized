# Clause Header Optimization - Analysis & Implementation

**Date:** 2026-02-02
**Goal:** Reduce clause memory footprint and improve cache efficiency

---

## Current Clause Structure

```c
struct clause {
  unsigned glue : 19;        // LBD score
  bool garbage : 1;
  bool quotient : 1;
  bool reason : 1;
  bool redundant : 1;
  bool shrunken : 1;
  bool subsume : 1;
  bool swept : 1;
  bool vivify : 1;
  unsigned used : 5;
  unsigned searched;         // Search position
  unsigned size;             // Number of literals
  unsigned lits[3];          // First 3 literals inline
};
```

**Size**: 24 bytes per clause
**For f1.cnf**: ~948K ternary clauses = 21.7 MB

---

## Optimization Option 1: Compact Header (16-bit fields)

### Idea
```c
struct clause_compact {
  uint32_t flags;      // All bit fields packed
  uint16_t size;       // 65K max (enough for all real clauses)
  uint16_t searched;   // 65K max
  unsigned lits[3];    // 12 bytes
};
```

**Size**: 20 bytes (saves 4 bytes = 17%)
**Memory saved**: 3.62 MB for f1.cnf

### Problems

1. **Requires size < 65536** - OK for SAT instances but limits flexibility
2. **Changes many files** - Every place that accesses `c->size` or `c->searched`
3. **Byte order issues** - Need careful packing/unpacking of flags
4. **Register pressure** - 16-bit values need extension to 32-bit for arithmetic

### Estimated Effort
- **Time**: 4-6 hours to change all clause accesses
- **Risk**: Medium (affects core data structure)
- **Gain**: 2-3% from better cache usage

---

## Optimization Option 2: Cache Line Alignment ✅ RECOMMENDED

### The Real Problem

Clauses are allocated sequentially in arena but NOT cache-line aligned:
```
Address 0:  [Clause1: 24 bytes]
Address 24: [Clause2: 24 bytes]  <-- Spans 2 cache lines!
Address 48: [Clause3: 24 bytes]
```

When CPU fetches a clause, it may need **2 cache line loads** (128 cycles each)!

### The Solution

Align clauses to cache line boundaries (64 bytes):
```
Address 0:   [Clause1: 24 bytes + 40 padding]
Address 64:  [Clause2: 24 bytes + 40 padding]  <-- Each in its own cache line!
Address 128: [Clause3: 24 bytes + 40 padding]
```

### Implementation

```c
// In clause.c
static inline size_t kissat_bytes_of_clause (unsigned size) {
  const size_t res = sizeof (clause) + (size - 3) * sizeof (unsigned);

#ifdef KISSAT_ALIGN_CLAUSES
  // Align to 64-byte cache lines
  return (res + 63) & ~63;
#else
  return kissat_align_ward (res);  // Original: 8-byte alignment
#endif
}
```

### Trade-offs

**Pros:**
- ✅ Simple - only one function to change
- ✅ Guaranteed one cache line per clause access
- ✅ No struct changes needed
- ✅ Easy to enable/disable

**Cons:**
- ❌ Wastes memory (40 bytes padding per clause)
- ❌ For f1.cnf: +36 MB memory usage
- ❌ Might not fit in cache anyway

### Verdict: NOT GOOD FOR F1.CNF

F1.cnf already has memory pressure. Adding 36MB won't help.

---

## Optimization Option 3: Hot/Cold Field Separation ✅ BEST FOR F1.CNF

### The Issue

During propagation, we access:
- `c->garbage` - ALWAYS (check if clause deleted)
- `c->size` - ALWAYS (find end of literals)
- `c->searched` - ALWAYS (resume search)
- `c->lits` - ALWAYS (access literals)

But we DON'T access:
- `c->glue` - only during reduction
- `c->reason` - only during conflict analysis
- `c->redundant` - only during GC
- `c->used` - only during reduction
- Other flags - rarely

### The Optimization

Split clause into HOT (frequently accessed) and COLD (rarely accessed) parts:

```c
// HOT: Read on every propagation
struct clause_hot {
  bool garbage : 1;
  unsigned size : 31;      // 31 bits enough (2B literals max)
  unsigned searched;
  unsigned lits[3];
};  // 20 bytes

// COLD: Read only during GC/reduction/analysis
struct clause_cold {
  unsigned glue : 19;
  unsigned used : 5;
  bool reason : 1;
  bool redundant : 1;
  bool shrunken : 1;
  bool subsume : 1;
  bool swept : 1;
  bool vivify : 1;
  bool quotient : 1;
};  // 4 bytes

// Full clause
struct clause {
  clause_hot hot;
  clause_cold cold;
};
```

### How It Helps

1. **Propagation only loads 20 bytes** (not 24)
2. **Better cache line usage**: 3 clauses per cache line (vs 2.67)
3. **Prefetching more effective**: Get more useful data per fetch

### Implementation Complexity

**Moderate** - Need to update:
- All `c->glue` → `c->cold.glue`
- All `c->redundant` → `c->cold.redundant`
 - etc.

**Estimated**: 50-100 file changes

### Verdict: GOOD IDEA BUT TOO MUCH WORK

Gain: 2-4%
Effort: 8+ hours
Risk: Medium

---

## Optimization Option 4: Prefetch Next Clause ✅ EASIEST!

Instead of changing clause structure, just prefetch the NEXT clause:

```c
// In proplit.h propagation loop
if (!binary) {
  watch tail = *q++ = *p++;
  const reference ref = tail.raw;

  // PREFETCH NEXT LARGE CLAUSE
  // Start loading it while we process current clause
  if (p + 1 < end_watches && !p[0].type.binary) {
    const reference next_ref = p[1].raw;
    __builtin_prefetch(arena + next_ref, 0, 3);
  }

  clause *const c = (clause *)(arena + ref);
  // ... process clause (50-100 cycles)
  // By now, next clause should be in cache!
}
```

### Why This Works

- **Clause processing time**: ~50-100 cycles
- **Prefetch latency**: ~100-200 cycles for L3→L1
- **By the time we finish current clause, next is ready!**

### Benefits

- ✅ **Zero code changes** except propagation loop
- ✅ **No memory overhead**
- ✅ **Easy to test** (add 5 lines, compile, test)
- ✅ **Low risk** (prefetch hints never cause errors)

### Expected Gain

For f1.cnf with 59% ternary clauses:
- **Before**: Load clause → wait 100 cycles → process
- **After**: Load clause → process → next already ready
- **Speedup**: 5-8% by hiding memory latency

---

## Recommendation: Implement Option 4

**Immediate action**: Add clause prefetching to propagation loop

**Implementation time**: 15 minutes
**Expected gain**: 5-8%
**Risk**: Very low

This is the **best effort-to-gain ratio** for f1.cnf.

---

## Summary

| Option | Gain | Effort | Risk | Memory | Verdict |
|--------|------|--------|------|--------|---------|
| 1. Compact header (16-bit) | 2-3% | 6h | Medium | -3.6 MB | Too much work |
| 2. Cache line alignment | 3-5% | 1h | Low | +36 MB | Too much memory |
| 3. Hot/cold separation | 2-4% | 8h | Medium | -4 MB | Too much work |
| 4. Prefetch next clause | **5-8%** | **15min** | **Low** | **0** | **✅ DO THIS** |

**Let's implement Option 4!**
