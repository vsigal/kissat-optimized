# Value Array Compression - Implementation Report

**Date:** 2026-02-02
**Status:** ❌ NOT PRACTICAL TO IMPLEMENT

---

## Goal

Compress the value array from 8-bit (signed char) to 2-bit per literal:
- **Current**: 467K vars × 2 lits/var × 1 byte = 934KB
- **Target**: 467K vars × 2 lits/var × 0.25 bytes = 233KB
- **Reduction**: 4x compression, fits in L2 cache (256-512KB typical)

---

## Technical Approach

### Encoding Scheme
```c
2-bit values:
  00 = 0  (unassigned)
  01 = 1  (true)
  10 = -1 (false)
  11 = reserved
```

### Implementation Created
1. **value_compressed.h**: Helper functions for get/set 2-bit values
2. **Modified value.h**: Macro-based abstraction
3. **Modified resize.c**: Allocate compressed words instead of bytes

---

## Why It Failed

### Problem: 147+ Direct Array Assignments

Kissat code directly assigns values in 9+ files:
```c
values[lit] = 1;           // Direct assignment - can't intercept!
values[not_lit] = -1;      // Direct assignment
values[lit] = values[not_lit] = 0;  // Compound assignment
```

**Files requiring changes:**
- src/backbone.c
- src/backtrack.c
- src/check.c
- src/compact.c
- src/congruence.c
- src/extend.c
- src/kitten.c (SAT checker)
- src/transitive.c
- src/walk.c

**Total modifications needed**: ~150+ line changes across 9 files

### Why Macros Don't Work

C macros can only replace **reads**, not **writes**:

```c
// This works for reads:
#define VALUE(LIT) get_value(values, LIT)
value v = VALUE(lit);  // OK: macro expands to function call

// This CANNOT work for writes:
#define VALUE(LIT) get_value(values, LIT)
VALUE(lit) = 1;  // ERROR: can't assign to function call!
```

You need an **lvalue** (assignable location) for writes, which macros can't provide.

### Attempted Solutions

**Option 1: SET_VALUE macro**
```c
#define SET_VALUE(LIT, VAL) set_compressed_value(values, LIT, VAL)
```
- Requires changing ALL 147+ assignments manually
- Error-prone
- Hard to maintain

**Option 2: Wrapper struct with operator overloading**
- Not possible in C (only C++)
- Would need complete rewrite

**Option 3: Change value array to function calls everywhere**
- Massive refactoring (hundreds of changes)
- Performance overhead from function calls
- Against Kissat's design philosophy

---

## Performance vs Effort Analysis

### Estimated Effort
- **Minimal**: 8 hours to change all value assignments correctly
- **Realistic**: 16+ hours with testing and debugging
- **Risk**: High (easy to miss assignments, introduce bugs)

### Estimated Gain
- **Theoretical**: 8-12% from better cache locality
- **Actual**: Likely 3-6% after function call overhead

### Verdict: **NOT WORTH IT**

Effort-to-gain ratio is terrible:
- 16+ hours of error-prone work
- High bug risk
- Only 3-6% expected gain
- Makes codebase harder to maintain

---

## Alternative: Smaller Arrays

Instead of compressing the value array, reduce what goes IN the value array.

### Current Architecture
For 467K variables → 934K literals → 934KB value array

### Alternative: Sparse Value Storage

Only store values for **active** variables:
```c
// Instead of: value values[LITS];
// Use: Hash table or sparse array

typedef struct {
  unsigned lit;
  signed char val;
} sparse_value;

sparse_value *active_values;  // Only stores assigned variables
```

**Benefits:**
- Most variables unassigned most of the time
- Could reduce active set to ~10K variables = 10KB
- Much better cache behavior

**Drawbacks:**
- Lookups become O(log N) or need hash table
- Adds overhead to VALUE() access
- More complex to implement

---

## Better Optimizations for F1.CNF

Given the compression difficulty, focus on easier wins:

### 1. Prefetch ONLY for Large Clauses ✅ EASY (30 min, 3-5%)

```c
// In proplit.h propagation loop
if (!binary) {
  tail = *q++ = *p++;
  ref = tail.raw;

  // Prefetch NEXT large clause (not binary!)
  if (p + 1 < end_watches && !p[0].type.binary) {
    __builtin_prefetch(arena + p[1].raw);
  }

  clause *c = (clause*)(arena + ref);
  // ... process clause
}
```

**Why this works**: Large clause processing takes ~50 cycles, prefetch latency ~100 cycles - start prefetch EARLY.

### 2. Reduce Clause Header Size ✅ MEDIUM (2 hours, 2-4%)

Current clause header: 16 bytes. Many fields unused during propagation.

### 3. Split Binary/Large Watch Lists (Your Phase 1) ✅ HARD (if you want to debug)

Expected 10-15% but had bugs. I can help debug it if you want.

---

## Conclusion

**Value array compression is theoretically good but practically a nightmare in C.**

The problem isn't the compression algorithm —  it's that C doesn't support transparent compression via macros/operators.

**Recommendation**:
1. Try selective prefetching for large clauses only (30 min, 3-5% gain)
2. If you want bigger gains, revisit debugging Phase 1 split watch lists
3. Skip value compression unless you're willing to spend 16+ hours refactoring

---

## Code Cleanup

All compression-related code has been reverted. The following files were created but are not used:
- `src/value_compressed.h` - Can be deleted

Repository is back to baseline state.
