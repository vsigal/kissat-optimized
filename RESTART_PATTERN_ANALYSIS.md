# Restartint Pattern Analysis

## Hypothesis: optimal restartint increases with f-number

### Test Results

| File | Best restartint | Conflicts/sec | Notes |
|------|-----------------|---------------|-------|
| f1.cnf | ~40 | 294,734 | Higher than baseline (25) |
| f2.cnf | ~75 | 723,998 | Higher than baseline (25) |
| f4.cnf | ~100 | 659,043 | Higher than baseline (25) |
| f8.cnf | ~100 | 885,207 | Same as f4, not higher |

### Pattern Observation

There IS a pattern, but it's not strictly linear with f-number:

```
f1 → optimal ~40 (1.6× baseline)
f2 → optimal ~75 (3× baseline)  
f4 → optimal ~100 (4× baseline)
f8 → optimal ~100 (4× baseline) ← Plateaus here
```

### Conclusion

The optimal restartint increases with problem hardness, but **plateaus around 100**.
Beyond f4, increasing restartint further doesn't help and may hurt performance.

### Recommended Formula

```
optimal_restartint ≈ min(100, 25 × log2(f_number))

Or simpler:
- f1: 40
- f2: 75  
- f4+: 100
```

### o-series Pattern

For o*.cnf files, the pattern is different - they tend to work best with LOWER
restartint values (20-30), suggesting they're easier instances that benefit
from more aggressive restarting.

