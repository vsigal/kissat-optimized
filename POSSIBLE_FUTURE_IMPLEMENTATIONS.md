# Possible Future Implementations

This document records ideas for future enhancements that have been analyzed but not yet implemented.

---

## Idea: Dynamic Restartint Adjustment

### Status
**Analyzed** ✓ | **Implemented** ✗ | **Priority**: Medium-High

### Summary
Dynamically adjust the `--restartint` parameter during runtime based on collected performance metrics. This would be a "meta-adaptive" layer above the existing adaptive restart logic.

### Background & Motivation

Current implementation:
- `--restartint` is set once at startup
- `adaptive_restart_delta()` adjusts interval by up to 3x based on glue/vivification
- But the **base value** remains fixed

Observation from f2.cnf testing:
```
restartint=25  → 125s  (too frequent restarts)
restartint=50  → 108s  (still too frequent)
restartint=100 →  94s  ✓ OPTIMAL
restartint=125 → 105s  (too infrequent)
restartint=150 → 138s  (way too infrequent)
```

The "sweet spot" exists but is **problem-dependent**. Static tuning requires:
1. Multiple test runs (time-consuming)
2. Prior knowledge of problem difficulty
3. Trade-off between exploration and exploitation

### Theoretical Foundation

**Key Insight**: Problem characteristics change during solving:
- **Early phase**: Many unit propagations, shallow search
- **Mid phase**: Deep search, complex learned clauses
- **Late phase**: Either converging or stuck in hard region

Different phases may benefit from different restartint values!

### Proposed Algorithm

#### Phase 1: Metric Collection (Continuous)
Track metrics in sliding windows:
```c
struct metric_window {
    double cps;              // Conflicts per second
    double avg_level;        // Average decision level
    double avg_glue;         // Average fast glue
    uint64_t conflicts;      // Total conflicts in window
    double duration;         // Window duration in seconds
};

// Maintain 3 windows: short (5k), medium (20k), long (100k conflicts)
```

#### Phase 2: Trend Detection (Every N conflicts)
```c
void analyze_and_adjust_restartint(solver) {
    // Only adjust every 10,000 conflicts
    if (CONFLICTS % 10000 != 0) return;
    
    // Calculate recent trends
    short_cps = get_window_cps(5000);
    medium_cps = get_window_cps(20000);
    
    // Detect problem phase
    if (short_cps > medium_cps * 1.3) {
        // Getting faster - problem easier than expected
        trend = EASIER;
    } else if (short_cps < medium_cps * 0.7) {
        // Getting slower - problem harder than expected
        trend = HARDER;
    } else {
        trend = STABLE;
    }
    
    // Apply adjustment with hysteresis
    switch (trend) {
        case EASIER:
            if (current_restartint > 50) {
                new_int = current_restartint - 25;
                LOG("Detected easier region, reducing restartint: %d → %d",
                    current_restartint, new_int);
            }
            break;
            
        case HARDER:
            if (current_restartint < 150) {
                new_int = current_restartint + 25;
                LOG("Detected harder region, increasing restartint: %d → %d",
                    current_restartint, new_int);
            }
            break;
            
        case STABLE:
            // No change
            break;
    }
    
    // Apply change
    if (new_int != current_restartint) {
        SET_OPTION(restartint, new_int);
        // Change takes effect on next restart limit calculation
    }
}
```

#### Phase 3: Validation & Rollback
```c
// Track performance before/after change
struct adjustment_record {
    uint64_t conflicts_at_change;
    int old_restartint;
    int new_restartint;
    double cps_before;
    double cps_after;
    bool was_improvement;
};

// If last adjustment made things worse, roll back
if (last_adjustment.worsened_performance()) {
    LOG("Last adjustment hurt performance, rolling back");
    SET_OPTION(restartint, last_adjustment.old_restartint);
}
```

### Integration Points

#### Option 1: Hook into `kissat_restart()`
```c
// In restart.c
void kissat_restart(solver) {
    // ... existing code ...
    
    // Add dynamic adjustment check
    if (GET_OPTION(dynamic_restartint)) {
        analyze_and_adjust_restartint(solver);
    }
    
    kissat_update_focused_restart_limit(solver);
}
```

**Pros**: Called at natural synchronization point
**Cons**: Only triggered when restart happens

#### Option 2: Hook into `kissat_analyze()`
```c
// In analyze.c
int kissat_analyze(solver, conflict) {
    // ... existing code ...
    
    // Periodic check independent of restarts
    if (GET_OPTION(dynamic_restartint) && CONFLICTS % 5000 == 0) {
        analyze_and_adjust_restartint(solver);
    }
}
```

**Pros**: More responsive, not tied to restart frequency
**Cons**: Called very frequently - need efficient check

#### Option 3: Separate Timer Thread (Advanced)
```c
// Background thread checks every 10 seconds
void* metric_monitor_thread(void* arg) {
    while (solver_running) {
        sleep(10);
        analyze_and_adjust_restartint(solver);
    }
}
```

**Pros**: Time-based, not conflict-based
**Cons**: Thread safety, complexity

### Challenges & Mitigations

| Challenge | Risk | Mitigation |
|-----------|------|------------|
| **Oscillation** | Rapidly flip-flopping between values | Hysteresis: require 2-3 consistent measurements before change |
| **Delayed effect** | Change only affects future restarts | Large window sizes (10k+ conflicts) to amortize delay |
| **Phase confusion** | Simplify phase vs Search phase behave differently | Disable adjustments during simplify (`!solver->probing`) |
| **Mode switching** | Stable vs Focused modes need different handling | Track mode changes, reset to default on switch |
| **Wrong direction** | Adjustment makes performance worse | Track "regret" - rollback if CPS decreases |
| **Overhead** | Metric tracking adds CPU cost | Use existing statistics, minimal extra computation |

### Parameters to Tune

```c
// New options to add to options.h
OPTION(dynamic_restartint, 0, 0, 1, "enable dynamic restartint adjustment")
OPTION(dynint_window_short, 5000, 1000, 20000, "short metric window (conflicts)")
OPTION(dynint_window_medium, 20000, 5000, 50000, "medium metric window")
OPTION(dynint_adjust_threshold, 0.3, 0.1, 0.5, "trend threshold for adjustment (ratio)")
OPTION(dynint_min, 50, 25, 100, "minimum dynamic restartint")
OPTION(dynint_max, 150, 100, 300, "maximum dynamic restartint")
OPTION(dynint_step, 25, 10, 50, "adjustment step size")
```

### Relationship to Existing `predict_restartint.py`

The Python script provides **offline prediction**:
```bash
python3 predict_restartint.py f2.cnf 30
# → Predicts optimal restartint based on 30s sample
```

Dynamic adjustment provides **online adaptation**:
- Starts with default or predicted value
- Continuously refines based on actual performance
- Adapts to problem phase changes

**Synergy**: Use script for initial guess, dynamic for refinement:
```bash
# 1. Get initial prediction
PREDICTED=$(python3 predict_restartint.py problem.cnf 30 | grep "PREDICTED" | awk '{print $4}')

# 2. Start solver with dynamic adjustment enabled
./kissat --restartint=$PREDICTED --dynamic-restartint=1 problem.cnf
```

### Expected Benefits

1. **No manual tuning required**: Eliminates need for parameter sweeps
2. **Problem-agnostic**: Works across different CNF types
3. **Phase-aware**: Adapts as problem structure changes
4. **Self-correcting**: Rolls back bad decisions

### Validation Strategy

Before full implementation, validate with:

1. **Simulation on f2.cnf**:
   - Log what adjustments would be made
   - Compare to optimal static value
   
2. **A/B testing framework**:
   ```c
   // Run both static and dynamic in parallel (two solver instances)
   // Compare performance on same problem set
   ```

3. **Regression testing**:
   - Ensure no degradation on existing benchmarks
   - Check f1-f16 series performance

### Alternative Approaches (Not Recommended)

| Approach | Why Not Recommended |
|----------|---------------------|
| Gradient descent on restartint | Too noisy, local minima |
| Genetic algorithm | Overkill, slow convergence |
| Fixed schedule (e.g., increase over time) | Ignores problem structure |
| RL-based (Q-learning) | Too complex, hard to debug |

### Conclusion

Dynamic restartint adjustment is **feasible and promising**. The existing architecture supports it with minimal changes. Main risks are oscillation and delayed feedback, both manageable with proper windowing and hysteresis.

**Recommended for implementation** when:
- Core solver performance is stable
- Benchmark suite is comprehensive
- Time available for parameter tuning

---

## Related Tools Reference

### predict_restartint.py
Location: `/home/vsigal/src/kissat-optimized/predict_restartint.py`

Purpose: Offline prediction of optimal restartint based on early runtime metrics.

Usage:
```bash
python3 predict_restartint.py <cnf_file> [monitor_seconds]
```

Key metrics used:
- Conflicts per second (CPS)
- Decision level
- CPS trend (2nd half vs 1st half)

Prediction formula:
```
BASE = 100
IF CPS > 18000:   BASE -= 25
IF CPS < 6000:    BASE += 25
IF level > 100:   BASE += 20
IF trend < 0.8:   BASE += 20
CLAMP to [50, 150]
ROUND to nearest 25
```

Accuracy: Within ±25 of optimal for f2.cnf (predicted 125, optimal 100).

Future integration: Could provide initial value for dynamic adjustment.

---

## Other Ideas (Brief)

### 1. Clause Quality Prediction
Use machine learning to predict which learned clauses will be useful, delete others earlier.

### 2. Parallel Portfolio with Dynamic Work Stealing
Multiple solver instances with different configs, share learned clauses.

### 3. Phase Saving with Conflict Analysis
Enhanced phase saving that considers conflict reasons, not just satisfiability.

### 4. GPU-Accelerated Clause Learning
Offload conflict analysis to GPU for massive parallelism.

---

*Last updated: 2026-02-07*
*Document version: 1.0*
