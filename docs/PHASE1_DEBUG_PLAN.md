# Phase 1: Split Watch Lists - Clean Implementation Plan

**Goal:** Split binary and large watches into separate lists to eliminate branch mispredictions

**Your Previous Attempt:** Worked on small instances, segfaulted on large instances (>10K vars)

---

## The Core Idea

Instead of:
```c
watches[lit] = [BIN, BIN, LARGE_HEAD, LARGE_TAIL, BIN, ...]
```

Use:
```c
struct watch_lists {
  watches binary;  // Only binary watches (1 word each)
  watches large;   // Only large watch pairs (2 words each)
};

watch_lists watches[LITS];
```

##likely Bug from Previous Attempt

From your FINAL_STATUS.md, the crash was likely:
1. **Vectoraccounting mismatch** - "freed != usable" warning
2. **Memory allocation size** - Fixed CREALLOC but may have other issues
3. **Off-by-one in large instances** - Works for small, fails for big

---

## Clean Implementation Strategy

Instead of modifying the entire codebase, let's do aMinimal, focused implementation:

### Step 1: Test Current Baseline
Measure current performance to have a solid comparison point.

### Step 2: Add Split Structure (Minimal Change)
Only modify the absolute minimum files:
1. `src/internal.h` - Add watch_lists type
2. `src/watch.h` - Macros for BINARY_WATCHES/LARGE_WATCHES
3. `src/resize.c` - Allocate both lists
4. `src/proplit.h` - Split propagation loop
5. `src/watch.c` - Update watch addition/removal

### Step 3: Debug Incrementally
Test after EACH change:
- Compile
- Test on small instance (3 vars)
- Test on medium instance (100 vars)
- Test on larger instance (10K vars)
- Only then test on f1.cnf

### Step 4: Fix Issues As They Arise
Common issues to watch for:
- Vector size calculations
- Literal indexing (2x for pos/neg)
- Watch iterator offsets
- Garbage collection

---

## Alternative: Simpler Optimization

If full split is too complex, try **SIMPLER** version:

### Binary-First Processing (No Structure Change!)

Just reorder loop iteration without changing data structures:

```c
// In proplit.h - Process binary watches first in a single pass
clause *PROPAGATE_LITERAL(...) {
  // PASS 1: Process ALL binary watches first
  for (p = begin; p != end; p++) {
    if (!p->type.binary) continue;  // Skip large
    // ... process binary ...
  }

  // PASS 2: Process ALL large watches
  for (p = begin; p != end; p++) {
    if (p->type.binary) continue;  // Skip binary
    // ... process large ...
  }
}
```

**Pros:**
- ZERO structural changes
- ZERO memory allocation changes
- ZERO vector accounting issues
- Still eliminates branch mispredictions in each loop
- Still gets cache benefits

**Cons:**
- Iterates watch list twice (but still sequential)
- Slightly more overhead than full split

**Expected Gain:** 8-12% (vs 10-15% for full split)

**Implementation Time:** 30 minutes (vs 8-16 hours for full split)

**Risk:** Very low (vs Medium-High for full split)

---

## Recommendation

**Try the SIMPLER two-pass version first:**

1. No struct changes
2. No memory changes
3. Just reorder the propagation loop
4. 30 minutes to implement
5. 8-12% expected gain
6. If it works, consider full split later if you need more

This avoids ALL the bugs you hit before while still getting most of the benefit!

---

## Next Steps

Do you want to:
1. **Try the simple two-pass version** (30 min, low risk, 8-12% gain)
2. **Debug the full split version** (need to find your old code, 4-8 hours)
3. **Implement full split from scratch** (8-16 hours, higher risk)

I recommend #1 - let's try the simple version first!
