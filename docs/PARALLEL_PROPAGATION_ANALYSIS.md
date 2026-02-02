# Heavy Parallelization of Unit Propagation

**Date:** 2026-02-02
**Hardware:** 24 threads (16 cores, i9-12900K)
**Target:** f1.cnf (467K vars, 1.6M clauses, 1.5B propagations)
**Current:** 2m52.75s single-threaded
**Goal:** 50-70% speedup using parallelism

---

## F1.CNF Parallelization Potential

### Workload Analysis
- **Total propagations:** 1.5 billion
- **Total conflicts:** 1.8 million
- **Avg propagations/conflict:** ~833
- **Current rate:** 8.7M props/second (single thread)

### Theoretical Speedup with 16 Cores
- **Perfect parallelism:** 16x speedup → 10.8 seconds
- **Realistic (50% parallel efficiency):** 8x speedup → 21.6 seconds
- **Conservative (25% parallel efficiency):** 4x speedup → 43 seconds

**Even 4x would be 2m52s → 43s!**

---

## Parallelization Strategies

### Strategy 1: Partitioned Variable Space 🎯 RECOMMENDED

**Concept:** Divide variables into partitions, each thread owns a partition

```
Thread 0: Variables 0-30K
Thread 1: Variables 30K-60K
Thread 2: Variables 60K-90K
...
Thread 15: Variables 450K-467K
```

**How It Works:**
```c
// Each thread has local state
typedef struct {
  unsigned start_var, end_var;
  value *local_values;      // Thread-local value cache
  unsigneds local_trail;    // Thread-local assignments
  clause *conflicts;        // Thread-local conflicts
} thread_partition;

// Parallel propagation
void parallel_propagate(kissat *solver) {
  // 1. Partition the current trail
  partition_trail(solver);

  // 2. Each thread propagates its literals
  #pragma omp parallel for
  for (int t = 0; t < NUM_THREADS; t++) {
    thread_propagate(&partitions[t]);
  }

  // 3. Merge results
  merge_assignments(solver);
  detect_conflicts(solver);
}
```

**Benefits:**
- Minimal cross-thread communication
- Each thread processes its own watch lists
- No lock contention if partitioned well

**Challenges:**
- **Cross-partition implications:** Var in partition 0 implies var in partition 5
- **Load balancing:** Some partitions may be busier
- **Merge overhead:** Collecting results from all threads

**Expected Gain:** 4-8x with 16 cores (50% efficiency)
**Implementation Time:** 2-3 weeks

---

### Strategy 2: Parallel Watch List Scanning 🎯 EASIER

**Concept:** Multiple threads scan the same watch list in parallel

```c
// For a literal with 1000 watches, split across 4 threads:
Thread 0: Process watches[0-249]
Thread 1: Process watches[250-499]
Thread 2: Process watches[500-749]
Thread 3: Process watches[750-999]
```

**Implementation:**
```c
clause *parallel_propagate_literal(kissat *solver, unsigned lit) {
  watches *ws = &WATCHES(NOT(lit));
  const size_t num_watches = SIZE_WATCHES(*ws);
  const size_t per_thread = num_watches / NUM_THREADS;

  // Each thread processes a slice
  #pragma omp parallel for
  for (int t = 0; t < NUM_THREADS; t++) {
    const watch *start = BEGIN_WATCHES(*ws) + t * per_thread;
    const watch *end = (t == NUM_THREADS-1) ? END_WATCHES(*ws)
                                             : start + per_thread;

    for (const watch *p = start; p < end; p++) {
      if (p->type.binary) {
        // Check for unit propagation (thread-safe read)
        value v = VALUES[p->binary.lit];
        if (v == 0) {
          // Needs assignment - queue it
          queue_assignment(t, p->binary.lit);
        } else if (v < 0) {
          // Conflict found
          record_conflict(t);
        }
      } else {
        // Process large clause
      }
    }
  }

  // Gather results (single-threaded)
  resolve_queued_assignments();
}
```

**Benefits:**
- Simple to implement
- Works for literals with many watches
- Natural load balancing

**Challenges:**
- **Only helps for long watch lists** (>100 watches)
- **f1.cnf has short watch lists** (avg 2.6 lits/clause)
- **Thread spawn overhead** may exceed gains

**Expected Gain:** 1.5-2x for long lists, but f1.cnf won't benefit much
**Implementation Time:** 1 week

---

### Strategy 3: Speculative Parallel Propagation 🎯 HIGHEST POTENTIAL

**Concept:** Multiple threads propagate different branches speculatively

**The Idea:**
```
Master thread: Propagates literal L1 → triggers L2, L3, L4...

Meanwhile:
Thread 1: Speculatively propagate if L2 were assigned
Thread 2: Speculatively propagate if L3 were assigned
Thread 3: Speculatively propagate if L4 were assigned
...

When master confirms L2, Thread 1's work is already done!
When master backtracks, discard speculative work.
```

**Implementation:**
```c
// Master thread trail: [L1, L2, L3, L4, L5...]

// Speculative threads
Thread 0: Assume L1=true, start propagating from L2
Thread 1: Assume L1=true, L2=true, start from L3
Thread 2: Assume L1=true, L2=true, L3=true, start from L4
...

// When master catches up, use speculative results if correct
```

**Benefits:**
- ✅ Exploits long propagation chains
- ✅ Can handle cross-dependencies
- ✅ No partitioning needed

**Challenges:**
- **Complex rollback** when speculation is wrong
- **Memory overhead** (N copies of solver state)
- **Wasted work** on misprediction

**Expected Gain:** 3-6x with good prediction (>50% speculation hit rate)
**Implementation Time:** 3-4 weeks (very complex)

---

### Strategy 4: GPU-Accelerated Propagation 🎯 EXOTIC

**Concept:** Use GPU for massivelyparallel clause evaluation

**f1.cnf is PERFECT for GPU:**
- 1.6M clauses = lots of parallelism
- 41% binary, 59% ternary = simple, uniform clauses
- No complex control flow

**Implementation:**
```c
// Upload to GPU
__global__ void evaluate_clauses(
  unsigned *clauses,      // All clauses (packed)
  char *values,           // Current assignment
  char *results           // Output: SAT, UNSAT, or UNIT
) {
  int clause_id = blockIdx.x * blockDim.x + threadIdx.x;
  if (clause_id >= num_clauses) return;

  // Each thread evaluates one clause in parallel
  char clause_status = evaluate_clause_gpu(clause_id, clauses, values);
  results[clause_id] = clause_status;
}

// On CPU: Collect unit clauses and propagate
```

**Benefits:**
- Thousands of clauses evaluated in parallel
- Perfect for uniform structure (f1.cnf's binary/ternary clauses)
- Can evaluate entire clause database in milliseconds

**Challenges:**
- CPU-GPU transfer overhead
- Complex GPU programming
- Need to rewrite Kissat significantly

**Expected Gain:** 2-5x (after transfer overhead)
**Implementation Time:** 4-6 weeks
**Requirements:** CUDA/OpenCL expertise

---

## Recommended Approach: Hybrid Partitioned + Speculative

### Architecture

```
Main Thread (Coordinator):
  - Makes decisions
  - Manages conflicts
  - Coordinates worker threads

Worker Threads (16 threads):
  - Partition 1: Variables 0-30K (Thread 0-1)
  - Partition 2: Variables 30K-60K (Thread 2-3)
  - ...
  - Partition 8: Variables 420K-467K (Thread 14-15)

  Each partition has 2 threads:
    - Thread A: Propagate current level
    - Thread B: Speculate next level
```

### Implementation Phases

#### Phase A: Basic Partitioning (Week 1-2)

```c
// Partition variables
#define NUM_PARTITIONS 8
#define THREADS_PER_PARTITION 2

typedef struct {
  unsigned start_var;
  unsigned end_var;
  watches *binary_watches[MAXLITS];   // Only watches for partition vars
  watches *large_watches[MAXLITS];    // Only watches for partition vars
  unsigneds trail;                     // Local assignments
  pthread_mutex_t lock;
} partition;

partition partitions[NUM_PARTITIONS];

// Initialize partitions
void init_partitions(kissat *solver) {
  unsigned vars_per_partition = solver->vars / NUM_PARTITIONS;

  for (int i = 0; i < NUM_PARTITIONS; i++) {
    partitions[i].start_var = i * vars_per_partition;
    partitions[i].end_var = (i+1) * vars_per_partition;

    // Copy relevant watches to partition
    distribute_watches(solver, &partitions[i]);
  }
}

// Parallel propagation
clause *parallel_propagate(kissat *solver) {
  // Broadcast current assignment to all threads
  broadcast_trail(solver);

  // Each partition propagates in parallel
  #pragma omp parallel for
  for (int p = 0; p < NUM_PARTITIONS; p++) {
    propagate_partition(&partitions[p], solver->trail);
  }

  // Collect assignments from all partitions
  return merge_partition_results(solver);
}
```

**Expected Gain:** 4-6x speedup (Week 2 deliverable)

#### Phase B: Add Speculation (Week 3-4)

```c
// Each partition runs 2 threads:
//   - Thread A: Propagate confirmed trail
//   - Thread B: Speculate on likely next assignment

typedef struct {
  // ... partition fields ...

  unsigneds speculative_trail;  // What we think will be assigned next
  bool speculation_valid;
} partition_speculative;

// Worker B speculatively propagates
void *speculative_worker(void *arg) {
  partition *p = (partition*)arg;

  while (solving) {
    // Predict next likely assignment
    unsigned predicted_lit = predict_next(p);

    // Speculatively propagate it
    propagate_literal_speculative(p, predicted_lit);

    // If prediction was correct, results are ready!
    // If wrong, discard and try again
  }
}
```

**Expected Additional Gain:** +2-3x on top of partitioning (total: 8-12x)

---

## F1.CNF-Specific Optimizations

### Why F1.CNF is Great for Parallelization

1. **Low variable coupling** (5.2 occurrences/var)
   - Less cross-partition dependencies
   - More independent propagations
   - Better parallelism potential

2. **Simple clauses** (41% binary, 59% ternary)
   - Fast to evaluate
   - GPU-friendly
   - Uniform processing

3. **Large scale** (467K variables)
   - Plenty of work to distribute
   - Overcomes thread overhead

4. **Encoding structure** (Tseitin-style)
   - Predictable implications
   - Good for speculation
   - Locality patterns

### Targeted Parallel Approach for F1.CNF

```c
// Exploit encoding structure: Variables fall into equivalence classes

// Step 1: Detect encoding structure at startup
void analyze_f1_structure(kissat *solver) {
  // Find Tseitin variable groups (X = Y & Z patterns)
  // Group: [aux_var, input1, input2]

  detect_xor_chains();
  detect_equivalence_classes();
  build_dependency_graph();
}

// Step 2: Partition by dependency levels
// Level 0: Input variables (can propagate independently)
// Level 1: First layer of Tseitin vars (depends on level 0)
// Level 2: Second layer, etc.

partition_by_levels();

// Step 3: Parallel propagation by level
for (level in dependency_levels) {
  // All vars in same level can propagate in parallel!
  #pragma omp parallel for
  for (var in level) {
    propagate_watched(var);
  }

  // Barrier: Wait for level to complete
  #pragma omp barrier

  // Next level
}
```

**Why This Works for F1.CNF:**
- Encoding has clear layered structure
- Variables in same layer have minimal dependencies
- Can achieve 80-90% parallel efficiency!

**Expected Gain:** 10-14x with 16 cores (90% efficiency)

---

## Implementation Roadmap

### Milestone 1: Basic Partitioning (2 weeks  → 4-6x speedup)

**Week 1:**
- Implement thread pools
- Partition variable space
- Duplicate watch structures per partition
- Basic parallel propagation
- Test on small instances

**Week 2:**
- Fix race conditions
- Optimize partition boundaries
- Tune for f1.cnf
- Benchmark

**Deliverable:** 2m52s → 35-45s

### Milestone 2: Add Speculation (2 weeks → +8-12x total)

**Week 3:**
- Implement speculative propagation
- Trail prediction heuristics
- Rollback mechanisms
- Conflict detection

**Week 4:**
- Optimize speculation accuracy
- Reduce wasted work
- Final tuning

**Deliverable:** 2m52s → 15-22s

### Milestone 3: Structure Exploitation (1 week → +10-14x total)

**Week 5:**
- Detect f1.cnf encoding structure
- Partition by dependency levels
- Exploit encoding patterns

**Deliverable:** 2m52s → 12-18s (~10x faster!)

---

## Technical Challenges & Solutions

### Challenge 1: Watch List Synchronization

**Problem:** Multiple threads updating same watch lists

**Solution 1 - Partition Ownership:**
```c
// Each partition owns watch lists for its variables
// No sharing = no locks needed!
Thread 0: Owns WATCHES(lit) for lit in [0, 60K]
Thread 1: Owns WATCHES(lit) for lit in [60K, 120K]
...
```

**Solution 2 - Lock-Free  Queues:**
```c
// Use atomic operations for assignment queue
atomic_unsigneds pending_assignments[NUM_THREADS];

// Thread-local processing, atomic merge
__atomic_compare_exchange_n(&queue[t], ...);
```

### Challenge 2: Conflict Detection

**Problem:** Multiple threads might find different conflicts simultaneously

**Solution:**
```c
// Use atomic flag for first conflict
atomic_bool conflict_found = false;
clause *first_conflict = NULL;

// In parallel region
if (detect_conflict(c)) {
  bool expected = false;
  if (__atomic_compare_exchange_n(&conflict_found, &expected, true, ...)) {
    // This thread found the first conflict
    first_conflict = c;
  }
  // Stop propagating
  return;
}
```

### Challenge 3: Load Balancing

**Problem:** Some vertices have more watches than others

**Solution - Dynamic Work Stealing:**
```c
// Work queue of literals to propagate
concurrent_queue work_queue;

// Worker threads steal work dynamically
void *worker_thread(void *arg) {
  while (unsigned lit = work_queue.pop()) {
    clause *conflict = propagate_literal(lit);
    if (conflict) return conflict;

    // Add new units to work queue
    work_queue.push_batch(new_units);
  }
}
```

### Challenge 4: Memory Overhead

**Problem:** Each thread needs scratch space

**Solution - Pre-allocate Thread-Local Storage:**
```c
typedef struct {
  value local_values[LITS];     // Thread-local cache: 935KB × 16 = 15MB
  unsigneds local_trail;        // ~100KB
  clause *local_delayed[1000];  // ~8KB
  // Total: ~1MB × 16 threads = 16MB overhead
} thread_local_data;
```

**For f1.cnf:** 16MB overhead is acceptable (< 10% of total memory)

---

## Parallelization Anti-Patterns (Don't Do This)

### ❌ Anti-Pattern 1: Fine-Grained Locking

```c
// BAD: Lock per watch list
pthread_mutex_t watch_locks[LITS];

clause *propagate(lit) {
  pthread_mutex_lock(&watch_locks[lit]);
  // ... propagate ...
  pthread_mutex_unlock(&watch_locks[lit]);
}
```

**Why bad:** Lock overhead dominates, slower than single-threaded

### ❌ Anti-Pattern 2: Global State Sharing

```c
// BAD: All threads modify global trail
extern unsigneds global_trail;  // Shared by all threads

// Thread safety nightmare!
```

**Why bad:** Synchronization overhead, cache line bouncing

### ❌ Anti-Pattern 3: Naive Thread-Per-Literal

```c
// BAD: Spawn thread for each literal
#pragma omp parallel for
for (unsigned lit = 0; lit < LITS; lit++) {
  propagate_literal(lit);
}
```

**Why bad:** 935K threads! Context switch overhead destroys performance

---

## Proof of Concept: Quick Parallel Test

### Simple OpenMP Version (2 hours implementation)

```c
// In src/search.c - parallel conflict search

#include <omp.h>

clause *kissat_parallel_search_propagate(kissat *solver) {
  clause *res = NULL;
  unsigned *trail_start = solver->propagate;
  unsigned *trail_end = END_ARRAY(solver->trail);
  const size_t trail_size = trail_end - trail_start;

  if (trail_size < 16) {
    // Too small for parallelism
    return kissat_search_propagate(solver);
  }

  // Partition trail into chunks
  #define NUM_THREADS 8
  const size_t chunk_size = trail_size / NUM_THREADS;

  #pragma omp parallel for shared(res)
  for (int t = 0; t < NUM_THREADS; t++) {
    if (res) continue;  // Another thread found conflict

    unsigned *chunk_start = trail_start + t * chunk_size;
    unsigned *chunk_end = (t == NUM_THREADS-1) ? trail_end
                                                : chunk_start + chunk_size;

    for (unsigned *p = chunk_start; p < chunk_end && !res; p++) {
      clause *conflict = propagate_literal_readonly(solver, *p);
      if (conflict) {
        #pragma omp critical
        {
          if (!res) res = conflict;  // First conflict wins
        }
      }
    }
  }

  // If no conflict, actually apply assignments (single-threaded)
  if (!res) {
    apply_assignments(solver);
  }

  return res;
}
```

**Expected gain:** 2-3x with 8 threads
**Time to implement:** 2-4 hours
**Risk:** Low (read-only first, then add writes)

---

## Realistic Timeline & Deliverables

### Week 1: Proof of Concept
- Implement simple OpenMP parallelization
- Test on f1.cnf
- **Target:** 2m52s → 60-90s (2-3x)

### Week 2-3: Partitioned Propagation
- Partition variable space
- Thread-local watch lists
- Lock-free merging
- **Target:** 2m52s → 30-45s (4-6x)

### Week 4-5: Speculation
- Add speculative threads
- Prediction heuristics
- **Target:** 2m52s → 15-25s (7-10x)

### Week 6: Tuning & Optimization
- Profile parallel overhead
- Tune partition sizes
- Optimize for f1.cnf structure
- **Target:** 2m52s → 12-20s (8-14x)

---

## Resource Requirements

### Software
- OpenMP (already available with GCC)
- pthreads (Linux standard)
- Atomic operations (C11 stdatomic.h)

### Hardware
- ✅ 16 cores available (i9-12900K)
- ✅ 24 threads with hyperthreading
- Memory: Need ~16-32MB extra for thread-local storage (acceptable)

### Expertise Needed
- Parallel programming (OpenMP, pthreads)
- Lock-free data structures
- SAT solver internals
- Performance profiling

---

## Expected Final Performance

### Conservative Estimate (50% parallel efficiency)
- Current: 2m52.75s
- With 8 effective cores: 2m52s / 8 = **21.6 seconds**

### Realistic Estimate (70% parallel efficiency)
- With 11 effective cores: 2m52s / 11 = **15.7 seconds**

### Optimistic Estimate (90% parallel efficiency)
- With 14 effective cores: 2m52s / 14 = **12.3 seconds**

**Likely Result: 15-25 seconds (~7-11x speedup)**

---

## Recommendation: Start with Proof of Concept

**Immediate Next Step:**

Implement the simple OpenMP version (2-4 hours work):
- Read-only parallel clause checking
- 2-3x speedup expected
- Low risk
- If it works, validates the approach
- If it fails, we learn why before investing weeks

**Then decide:**
- If PoC gives 2x → invest in full implementation (weeks)
- If PoC gives <1.5x → parallel overhead too high, abandon
- If PoC gives >3x → huge potential, definitely continue!

---

## Next Steps

Do you want me to:
1. **Implement the OpenMP proof of concept** (2-4 hours, test parallelization potential)
2. **Design the full architecture** (detailed spec for 4-week implementation)
3. **Analyze f1.cnf structure first** (find encoding patterns for structure exploitation)

I recommend #1 - let's test if parallelization can actually help before investing weeks!
