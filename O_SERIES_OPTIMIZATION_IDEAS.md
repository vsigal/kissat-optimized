# Optimization Ideas for o-series CNF Files

Based on kissat --help analysis and earlier testing.

## Key Characteristics of o-series (from testing)
- Faster to solve (o15: ~12s, o13: ~44s vs f2: ~150s)
- Different structure than f-series
- Baseline parameters work well (no major improvement with tuning)
- Lower optimal restartint (20-30) vs f-series (40-100)

---

## Idea 1: Disable Expensive Preprocessing

**Observation**: o-series solves fast - preprocessing overhead may not be worth it.

**Options to try**:
```bash
# Disable initial preprocessing
./kissat --preprocess=false o15.cnf

# Or disable specific expensive preprocessing steps
./kissat --preprocesscongruence=false o15.cnf
./kissat --preprocessbackbone=false o15.cnf
./kissat --preprocessfactor=false o15.cnf
```

**Expected impact**: Reduce startup overhead for fast-solving instances.

---

## Idea 2: Faster Mode Switching

**Observation**: o-series may benefit from quicker transition to stable mode.

**Options**:
```bash
# Shorter focused mode before switching to stable
./kissat --modeinit=500 --modeint=500 o15.cnf

# Or always use focused mode (for SAT-like instances)
./kissat --stable=0 o15.cnf
```

**Rationale**: From --help: `--sat` config uses `--target=2 --restartint=50`

---

## Idea 3: More Aggressive Restarts (Lower restartint)

**Observation**: o-series work best with lower restartint (20-30).

**Options**:
```bash
# More aggressive restarts for fast instances
./kissat --restartint=20 --reduceint=1000 o15.cnf

# With adaptive enabled (default)
./kissat --restartint=20 o15.cnf
```

---

## Idea 4: Disable Expensive Inprocessing

**Observation**: o-series don't need heavy simplification during solving.

**Options to disable**:
```bash
# Disable vivification (clause strengthening)
./kissat --vivify=false o15.cnf

# Disable congruence closure
./kissat --congruence=false o15.cnf

# Disable AND gate extraction
./kissat --ands=false o15.cnf

# Disable equivalence extraction  
./kissat --equivalences=false o15.cnf
```

---

## Idea 5: Reduce Clause Database Maintenance

**Observation**: For fast solves, less maintenance overhead.

**Options**:
```bash
# Less frequent clause reduction
./kissat --reduceint=2000 o15.cnf

# Disable minimize (expensive)
./kissat --minimize=false o15.cnf

# Disable shrink
./kissat --shrink=0 o15.cnf
```

---

## Idea 6: SAT-Targeted Configuration

**Observation**: o-series might be satisfiable instances.

**Use predefined config**:
```bash
./kissat --sat o15.cnf
# Equivalent to: --target=2 --restartint=50
```

**Or UNSAT-targeted**:
```bash
./kissat --unsat o15.cnf
# Equivalent to: --stable=0
```

---

## Idea 7: Disable Lucky Assignments

**Observation**: For structured instances, lucky guesses may not help.

**Options**:
```bash
./kissat --lucky=false o15.cnf
./kissat --luckyearly=false o15.cnf
./kissat --luckylate=false o15.cnf
```

---

## Idea 8: Basic CDCL Mode

**Observation**: Strip down to basic CDCL for speed.

**Options**:
```bash
# Basic CDCL without advanced techniques
./kissat --plain o15.cnf

# Or even more basic (no restarts, minimize, reduce)
./kissat --basic o15.cnf
```

---

## Idea 9: Tune Decision Heuristics

**Observation**: Different decay for different instance types.

**Options**:
```bash
# Faster score decay (more focus on recent activity)
./kissat --decay=30 o15.cnf

# Or slower decay (more memory of past activity)
./kissat --decay=70 o15.cnf
```

---

## Idea 10: Phase Saving Tuning

**Observation**: Phase saving helps, but forced phases might hurt fast solves.

**Options**:
```bash
# Disable phase saving
./kissat --phasesaving=false o15.cnf

# Force initial phase
./kissat --forcephase=true --phase=true o15.cnf
```

---

## Idea 11: Disable Walk (Local Search)

**Observation**: Local search overhead for fast instances.

**Options**:
```bash
./kissat --walkinitially=false o15.cnf
# Note: --walk flag not exposed, controlled by walkeffort
./kissat --walkeffort=0 o15.cnf
```

---

## Idea 12: Tune Reorder Frequency

**Observation**: Variable reordering is expensive.

**Options**:
```bash
# Less frequent reordering
./kissat --reorderint=50000 o15.cnf

# Disable reordering
./kissat --reorder=0 o15.cnf
```

---

## Recommended Test Matrix for o-series

```bash
# Test 1: Minimal preprocessing
./kissat --preprocess=false o15.cnf

# Test 2: SAT-targeted
./kissat --sat o15.cnf

# Test 3: Basic CDCL
./kissat --basic o15.cnf

# Test 4: Aggressive restarts
./kissat --restartint=20 --reduceint=2000 o15.cnf

# Test 5: No vivify
./kissat --vivify=false o15.cnf

# Test 6: Combined fast config
./kissat --preprocess=false --vivify=false --minimize=false \
         --restartint=20 --modeinit=500 o15.cnf
```

---

## Success Metrics

Compare:
1. Wall clock time
2. Conflicts per second
3. Total conflicts to solution
4. Memory usage

Document which options help o-series specifically.
