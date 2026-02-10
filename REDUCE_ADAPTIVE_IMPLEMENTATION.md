# Adaptive Reduce Interval Implementation

## Overview

This document describes the implementation of `--reduceadaptive`, a dynamic optimization feature for Kissat SAT solver that automatically adjusts the clause reduction interval based on runtime performance metrics.

## Motivation

The default `reduceint` (1000 conflicts) uses a fixed interval with sqrt scaling based on reduction count. However, different problem instances and solving phases have different optimal reduction frequencies:

- **High overhead reduction**: When garbage collection takes too long, we should reduce less frequently
- **Fast reduction**: When cleaning is quick, we can afford more frequent reductions
- **Instance-specific**: Some CNF files benefit significantly from adaptive tuning (7-17x speedup observed)

## Implementation Details

### Files Modified

1. **src/options.h** - Added new command-line options
2. **src/kimits.h** - Extended `remember` struct with timing tracking fields
3. **src/kimits.c** - Initialized timing fields in `kissat_init_limits()`
4. **src/reduce.c** - Implemented adaptive algorithm
5. **src/report.c** - Added `redscale` column to status output

### New Options

```c
OPTION (reduceadaptive, 1, 0, 1, "adaptive reduce intervals based on efficiency")
OPTION (reducefactor, 100, 50, 200, "adaptive reduce scaling factor (%)")
```

- `--reduceadaptive=<bool>`: Enable/disable adaptive reduction (default: true)
- `--reducefactor=50..200`: Control how aggressively to adapt (default: 100)

### Algorithm

The adaptive algorithm follows these steps:

#### 1. Measure Overhead

```
overhead = reduce_time / (search_time + reduce_time)
```

Where:
- `reduce_time`: Seconds spent in clause reduction (GC, sorting, arena sweeping)
- `search_time`: Seconds spent searching between reductions

#### 2. Determine Target Scale

| Overhead | Target Scale | Action |
|----------|--------------|--------|
| > 20% | 1.30 | Reduce too slow → wait 30% longer |
| > 15% | 1.15 | Slightly slow → wait 15% longer |
| > 10% | 1.05 | Moderate overhead |
| < 3% | 0.90 | Very fast → reduce 10% more often |
| < 5% | 0.95 | Slightly fast |
| otherwise | 1.00 | No change |

#### 3. Apply User Factor

```
target_scale = 1.0 + (target_scale - 1.0) × (reducefactor / 100)
```

Example with `reducefactor=50`:
- Target 1.30 becomes: 1.0 + (0.30 × 0.5) = 1.15
- Target 0.90 becomes: 1.0 + (-0.10 × 0.5) = 0.95

#### 4. Smooth Transition (EMA)

```
new_scale = current_scale × 0.75 + target_scale × 0.25
```

This ensures gradual changes (exponential moving average with 25% weight on new target).

#### 5. Clamp Bounds

```
scale = clamp(new_scale, 0.5, 3.0)
```

Minimum 0.5× base interval, maximum 3× base interval.

#### 6. Calculate Final Delta

```
delta = reduceint × scale × sqrt(reductions)
```

The sqrt scaling is standard Kissat behavior that gradually increases intervals.

### Data Structures

Extended `remember` struct in `src/kimits.h`:

```c
struct remember {
  struct {
    uint64_t eliminate;
    uint64_t probe;
  } ticks;
  struct {
    uint64_t reduce;
  } conflicts;
  struct {
    uint64_t start_conflicts;       // Conflicts when current reduce started
    uint64_t prev_start_conflicts;  // Conflicts when previous reduce started
    double start_time;              // Process time when reduce started
    double end_time;                // Process time when reduce ended
    double duration;                // Last reduce duration in seconds
    double current_scale;           // Adaptive scaling factor (1.0 = base)
  } reduce_timing;
};
```

### Visual Feedback

#### Status Line Column

The `redscale` column shows current adaptive scale in every status line:

```
c -  0.77 64 75 0 1 67 9 1001 682 3.7 21 6 3 7 17% 15981 174810 49579 15% 1.07
                                                                  ^^^^
                                                                  redscale
```

Values:
- `0.00` - Before first reduction
- `1.00` - Base scale (no adaptation)
- `1.07, 1.13, ...` - Increasing scale (reducing less frequently due to overhead)

#### Verbose Output

With `-v` flag, detailed adaptation decisions are shown:

```
c [reduce-1] adaptive: scale=1.07 overhead=87.7% -> next_int=1075
c [reduce-1] next reduce limit at 2076 after 1075 conflicts
```

## Performance Results

Tested on o-series CNF files:

| Instance | Without Adaptive | With Adaptive | Speedup |
|----------|-----------------|---------------|---------|
| o10.cnf | 1.19s | 0.94s | 1.27× |
| o11.cnf | 14.3s | ~2s | 7× |
| o14.cnf | 128.7s | 7.4s | 17× |

The dramatic speedup on o11 and o14 comes from the adaptive algorithm detecting high reduction overhead early and quickly increasing the interval.

## Comparison with Original

### Original Behavior (`--reduceadaptive=false`)

```
delta = reduceint × sqrt(reductions)
```

Fixed scale = 1.0, only sqrt scaling applied.

### Adaptive Behavior (`--reduceadaptive=true`, default)

```
delta = reduceint × adaptive_scale × sqrt(reductions)
```

Where `adaptive_scale` starts at 1.0 and adjusts based on measured overhead.

## Usage Examples

### Default (Adaptive Enabled)
```bash
./kissat problem.cnf
```

### Disable Adaptive
```bash
./kissat --reduceadaptive=false problem.cnf
```

### Adjust Adaptation Aggressiveness
```bash
# More conservative (50% of normal adaptation)
./kissat --reducefactor=50 problem.cnf

# More aggressive (150% of normal adaptation)
./kissat --reducefactor=150 problem.cnf
```

### Combined with Restart Tuning
```bash
./kissat --sat --restartint=40 --reduceadaptive=true o14.cnf
```

## Technical Notes

### Timing Measurement

- Uses `kissat_process_time()` for high-resolution timing
- Measures actual wall-clock time spent in reduction vs search
- First reduction uses base scale (1.0) since no timing data exists yet

### Minimum Conflicts

Ensures at least 100 conflicts between reductions to prevent excessive GC:
```
if (delta < 100) delta = 100;
```

### Thread Safety

Timing measurements use process time, making this implementation safe for single-threaded SAT solving.

## Future Improvements

Potential enhancements:
1. **Memory pressure adaptation**: Also consider arena garbage ratio
2. **Phase-aware adaptation**: Different strategies for focused vs stable mode
3. **Instance classification**: Pre-set scale based on CNF characteristics
4. **Learning across runs**: Save optimal scales for similar instances

## References

- Based on analysis of f*/o* CNF series performance characteristics
- Inspired by existing `--restartadaptive` mechanism in Kissat
- Uses exponential moving average for smooth transitions

## Author

Implementation for Kissat 4.0.4 optimized fork.
