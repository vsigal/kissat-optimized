# Parameter Tuning Results for o13, o14, o15.cnf

## Executive Summary

Unlike f2.cnf which showed 42% improvement with tuning, the o*.cnf files work best with **baseline parameters** or minimal adjustments.

## Optimal Configurations

### o13.cnf
- **Best**: `--restartint=25 --reduceint=1000` (baseline)
- **Time**: 43.72 seconds
- **Result**: Baseline is optimal

### o14.cnf  
- **Best**: `--restartint=35 --reduceint=2000`
- **Time**: 81.66 seconds
- **vs Baseline**: 82.24s → 81.66s (-0.7%)
- **Result**: Minimal improvement

### o15.cnf
- **Best**: `--restartint=25 --reduceint=1000` (baseline)
- **Time**: 11.30 seconds
- **Result**: Baseline is optimal

## Usage Recommendations

```bash
# o13.cnf - Use default parameters
./kissat o13.cnf

# o14.cnf - Slight tuning helps
./kissat --restartint=35 --reduceint=2000 o14.cnf

# o15.cnf - Use default parameters
./kissat o15.cnf
```

## Key Insights

1. **Instance-specific tuning matters**: Different CNF families respond differently to parameters
2. **Baseline is good**: Kissat's defaults are well-tuned for many instances
3. **Long-running vs fast instances**: 
   - Long (f*.cnf): Benefit from less frequent restarts
   - Fast (o*.cnf): Work best with aggressive restarting

## Tested Configurations

Multiple combinations tested:
- restartint: 25, 30, 35, 40, 50, 60, 75, 100
- reduceint: 1000, 1500, 2000, 2500, 3000, 5000

Best results documented above.
