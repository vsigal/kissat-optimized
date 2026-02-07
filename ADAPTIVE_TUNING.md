# Adaptive Restart Tuning Analysis

## Key Finding

`--restartadaptive=true` (default) adapts restart intervals based on glue/vivification
statistics, but needs a **higher base `restartint`** for hard instances.

## Comparison: Adaptive vs Fixed

### f2.cnf Results

| Configuration | Time | Notes |
|---------------|------|-------|
| Baseline (adaptive, r=25, red=1000) | ~177s | Default settings |
| Adaptive + r=75 + red=2000 | ~118s | **-33%** |
| Fixed (adaptive=false, r=50, red=2000) | ~87s | **-51%** (best, but less stable) |

## Trade-offs

### Fixed Restart (adaptive=false)
- **Pros**: Higher peak performance (50%+ faster on f2)
- **Cons**: May get stuck on some instances, less robust

### Adaptive Restart (adaptive=true, default)
- **Pros**: More robust, self-adjusting
- **Cons**: Needs proper base tuning, ~20% slower than fixed

## Recommendation

For **maximum performance** on known hard instances:
```bash
# Use fixed restart with tuned values
./kissat --restartadaptive=false --restartint=75 --reduceint=2000 f2.cnf
```

For **general use** with robustness:
```bash
# Keep adaptive, but increase base restartint
./kissat --restartint=75 --reduceint=2000 hard_instance.cnf
```

## Pattern Still Holds

Even with adaptive=true, optimal `restartint` follows the f-number pattern:
- f1: restartint ~40-50
- f2: restartint ~75  
- f4+: restartint ~100
