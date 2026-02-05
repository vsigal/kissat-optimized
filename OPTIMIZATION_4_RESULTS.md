# Optimization #4: Variable Decision Cache - Results

## Summary

**MAJOR BREAKTHROUGH**: Variable Decision Cache achieved **24.6% speedup** on f1.cnf!

## Implementation

### Design
- 8-entry LRU cache for top decision candidates
- Cache populated from VSIDS score heap
- LRU eviction within the cache
- Cache invalidated on:
  - Variable assignment (decision made)
  - Score bumps (during conflict analysis)

### Code Changes
- `internal.h`: Added decision cache fields to solver struct
- `internal.c`: Initialize cache in kissat_init()
- `decide.c`: Cache fill/lookup/get functions
- `assign.c`: Invalidate cache on decision
- `bump.c`: Invalidate cache on score updates

## Results

### f1.cnf Performance
| Metric | Before | After | Improvement |
|--------|--------|-------|-------------|
| **Time** | 82.0s | **61.8s** | **-24.6%** |
| Consistency | - | ±0.02s | Excellent |

### Verification Runs
- Run 1: 61.82s
- Run 2: 61.79s  
- Run 3: 61.80s

### Cumulative Results
| Stage | Time | Improvement |
|-------|------|-------------|
| Baseline | 94.0s | - |
| After Prefetching | 85.0s | -9.6% |
| After Branch Hints | 84.2s | -1.0% |
| After Clause Tracking | 82.0s | -2.6% |
| **After Decision Cache** | **61.8s** | **-24.6%** |
| **TOTAL** | - | **-34.3%** |

## Why This Works

The VSIDS (Variable State Independent Decaying Sum) decision heuristic maintains a heap of variable scores. Finding the highest-scoring unassigned variable requires:
1. Getting max from heap
2. Checking if assigned
3. If assigned, popping and repeating

Most top-scored variables are already assigned, causing many heap operations. The cache stores the top 8 unassigned variables, avoiding repeated heap traversals.

## GitHub Commit

```
Commit: 1c2c71b
Message: Optimization #4: Variable Decision Cache
Repository: https://github.com/vigal/kissat-optimized
```

## Next Steps

Optimization #5: Memory Pool for Clauses
- Expected: 5-10% additional improvement
- Reduce malloc/free overhead
- Better cache locality for clauses
