# Optimization #1: Inline Binary Clause Storage - Analysis

**Date:** 2026-02-02
**Status:** ✅ ALREADY IMPLEMENTED IN KISSAT

---

## Discovery

After examining the Kissat source code to implement inline binary clause storage, I discovered that **Kissat already implements this optimization**!

### Evidence

1. **Watch Structure** (src/watch.h:38-44):
```c
union watch {
  watch_type type;
  binary_watch binary;     // Contains: 1 bit flag + 31 bits literal
  blocking_watch blocking; // For large clauses
  large_watch large;       // Contains: 1 bit flag + 31 bits reference
  unsigned raw;            // 32-bit total
};
```

2. **Binary Clause Creation** (src/clause.c:64-79):
```c
static reference new_binary_clause(...) {
  // ...
  kissat_watchbinary (solver, first, second);
  // ...
  return INVALID_REF;  // NO ARENA ALLOCATION!
}
```

Binary clauses return `INVALID_REF` - they don't allocate any arena space!

3. **Propagation** (src/proplit.h:82-93):
```c
if (binary) {
  if (blocking_value < 0) {
    res = kissat_binary_conflict(solver, not_lit, blocking);
  } else {
    kissat_fast_binary_assign(solver, ..., blocking, not_lit);
  }
}
```

Binary propagation uses `blocking` literal directly from the watch - no arena lookup!

---

## Current Architecture

### How Kissat Stores Clauses

**Binary clauses (2 literals):**
- Literal A watches literal B
- Literal B watches literal A
- Watch structure contains the other literal inline (31 bits)
- **Total storage**: 2 × 32 bits = 64 bits per binary clause
- **Zero arena allocation**

**Large clauses (3+ literals):**
- Stored in arena
- Two literals watch each other
- Watch structure contains arena reference (31 bits) + blocking literal
- **Total overhead**: 64 bits per watch pair + clause header + literals in arena

### Why This is Already Optimal for Binary

The current design for binary clauses is essentially perfect:
- No indirection needed
- No arena allocation
- Direct literal access
- Compact (64 bits total)

---

## Remaining Inefficiency: Mixed Processing

While binary storage is optimal, there's still a minor inefficiency in the **propagation loop mixing**:

### Current Loop Structure (src/proplit.h:71-99)

```c
while (p != end_watches) {
  const watch head = *q++ = *p++;                // Read watch
  const unsigned blocking = head.blocking.lit;  // Extract blocking lit
  const value blocking_value = values[blocking]; // ALWAYS lookup value
  const bool binary = head.type.binary;          // Check type
  watch tail;
  if (!binary)
    tail = *q++ = *p++;                          // Conditionally read tail
  if (blocking_value > 0)                        // Check if satisfied
    continue;
  if (binary) {
    // Fast path: 6 lines
  } else {
    // Slow path: arena lookup + clause processing (100+ lines)
  }
}
```

### The Inefficiency

1. **Branch on type** (line 76): CPU must predict whether watch is binary/large
2. **Conditional tail read** (line 78-79): Adds unpredictable branches
3. **Mixed iteration**: Binary and large watches interleaved = poor cache locality

### Performance Impact

For f1.cnf with 41% binary clauses:
- ~40% iterations are binary (fast)
- ~60% iterations are large (slow)
- Branch predictor struggles with 40/60 split
- Cache lines contain mix of binary/large watches

---

## Real Optimization Opportunity: Split Watch Lists

This is actually **your previous Phase 1 work** that you mentioned had issues!

### The Idea

Instead of:
```c
watches[lit] = [BIN, BIN, LARGE, LARGE, BIN, LARGE, ...]
```

Use:
```c
struct {
  watches binary;  // Only binary watches
  watches large;   // Only large watches
} split_watches[lit];
```

### Benefits

1. **No type checking**: Process all binary, then all large - 100% predictable
2. **Better cache locality**: Binary watches sequential in memory
3. **Simpler loops**: Separate loops optimized for each type
4. **No conditional tail read**: Binary loop never reads tail

### Expected Gain

- Eliminate 41% of type checks
- Perfect branch prediction (was ~50% accurate, now 100%)
- Better cache efficiency (sequential access)
- **Estimated: 10-15% speedup on f1.cnf**

---

## Why Phase 1 May Have Failed

You mentioned Phase 1 (split watch lists) had issues. Common problems:

1. **Vector accounting**: Tracking usable space with 2 vectors per lit instead of 1
2. **Garbage collection**: Need to handle both binary and large lists
3. **Watch removal**: Need to search correct list
4. **Clause shrinking**: Clause becoming binary needs to move from large→binary list

These are solvable but require careful implementation.

---

## Alternative Optimizations for F1.CNF

Since binary storage is already optimal and split lists had issues, focus on:

### A. Loop Unrolling for Binary Watches

```c
// Current
if (binary) {
  if (blocking_value < 0) conflict else assign;
}

// Optimized: Process binaries in batches of 4
while (has 4+ consecutive binaries) {
  // Unroll 4x, better instruction pipelining
  process_binary(watch[0]);
  process_binary(watch[1]);
  process_binary(watch[2]);
  process_binary(watch[3]);
}
```

**Expected**: 3-5% gain from better pipelining

### B. Value Array Prefetching

```c
while (p != end_watches) {
  const watch head = *p;

  // Prefetch blocking literal value for NEXT iteration
  if (p + 1 < end_watches) {
    unsigned next_blocking = (p+1)->blocking.lit;
    __builtin_prefetch(&values[next_blocking]);
  }

  // Process current
  blocking_value = values[head.blocking.lit]; // Should hit cache
  ...
}
```

**Expected**: 5-8% gain by hiding value lookup latency

### C. Blocking Literal Optimization

For large clauses, the "blocking literal" is supposed to be likely satisfied. We could:
- Track effectiveness of blocking literals
- Replace ineffective blocking literals more aggressively
- Use most recently assigned literal as blocker

**Expected**: 2-5% gain

---

## Recommendations

### For F1.CNF Specifically

Given that binary storage is already optimal, try these instead:

**Priority 1: Value Array Compression** (Optimization #4)
- Compress 467K variable array from 1.8MB to 230KB
- Would fit in L2 cache entirely
- Expected: 8-12% speedup
- Risk: Low (just change accessors)

**Priority 2: Value Prefetching** (Option B above)
- Hide memory latency for value lookups
- Expected: 5-8% speedup
- Risk: Very low (just adds prefetch hints)

**Priority 3: Loop Unrolling** (Option A above)
- Better instruction-level parallelism
- Expected: 3-5% speedup
- Risk: Very low

**Total expected: 16-25% speedup**

### Debugging Phase 1 (If You Want)

If you want to fix the split watch lists implementation:
1. Show me the error you're getting
2. I can help debug the vector accounting issue
3. Potential 10-15% gain if we fix it

---

## Summary

**Optimization #1 is already done!** Kissat's architects already implemented inline binary storage optimally.

The remaining opportunities are:
- **Not implemented**: Split watch lists (your Phase 1)
- **Low-hanging fruit**: Value prefetching, loop unrolling
- **Big win**: Value array compression (fits in cache)

Since you asked for "only Optimization #1", and it's already implemented, I recommend we pivot to:
1. **Value array compression** (biggest remaining opportunity)
2. **Value prefetching** (easiest to add, low risk)
3. **Debug Phase 1** (if you want to tackle the split lists bug)

What would you like to do next?
