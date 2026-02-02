# GPU-Accelerated Unit Propagation Design

**Target:** f1.cnf (467K vars, 1.6M clauses, 1.5B propagations)
**Hardware:** CUDA-capable GPU
**Goal:** 5-10x speedup on propagation (60-80% of total time)

---

## Architecture Overview

### CPU Responsibilities (Keep)
- Decision making (VSIDS/CHB scoring)
- Conflict analysis and learning
- Clause database management
- Restart/reduce heuristics
- Overall search strategy

### GPU Responsibilities (New)
- **Unit propagation ONLY**
- Clause evaluation (parallel)
- BCP (Boolean Constraint Propagation)
- Implication detection

---

## GPU Propagation Algorithm

### Data Structures on GPU

```c
// Upload once, update incrementally
typedef struct {
  // Clause database (read-only during propagation)
  unsigned *clause_data;      // Flat array: [size, lit1, lit2, ..., size, lit1, ...]
  unsigned *clause_offsets;   // Index into clause_data (1.6M entries)
  unsigned clause_count;

  // Watch lists (read-only during propagation)
  unsigned *watch_lists;      // Flat: [clause_id, clause_id, -1, clause_id, -1, ...]
  unsigned *watch_offsets;    // Start of each literal's watch list (935K entries)

  // Current assignment (updated each propagation)
  char *values;               // -1, 0, or 1 for each literal (935K bytes)

  // Output buffers (write by GPU)
  unsigned *units;            // Detected unit implications
  unsigned *conflicts;        // Detected conflicts
  unsigned unit_count;        // Atomic counter
  unsigned conflict_count;    // Atomic counter
} gpu_propagation_state;
```

### Kernel 1: Evaluate All Watched Clauses

```cuda
__global__ void evaluate_watched_clauses(
  unsigned lit,                  // The literal being propagated
  unsigned *clause_offsets,      // Clause database lookup
  unsigned *clause_data,
  unsigned *watch_offsets,       // Which clauses watch lit
  unsigned *watch_lists,
  char *values,                  // Current assignment
  unsigned *unit_buffer,         // Output: unit implications
  unsigned *conflict_buffer,     // Output: conflicts
  unsigned *unit_count,          // Atomic counter
  unsigned *conflict_count       // Atomic counter
) {
  // Each thread processes one watched clause
  int watch_idx = blockIdx.x * blockDim.x + threadIdx.x;

  unsigned not_lit = lit ^ 1;  // Opposite polarity
  unsigned watch_start = watch_offsets[not_lit];
  unsigned watch_end = watch_offsets[not_lit + 1];
  unsigned num_watches = watch_end - watch_start;

  if (watch_idx >= num_watches) return;

  // Get clause ID from watch list
  unsigned clause_id = watch_lists[watch_start + watch_idx];
  if (clause_id == 0xFFFFFFFF) return;  // Deleted watch

  // Load clause
  unsigned offset = clause_offsets[clause_id];
  unsigned size = clause_data[offset];
  unsigned *lits = &clause_data[offset + 1];

  // Evaluate clause
  int num_unassigned = 0;
  int unassigned_lit = -1;
  bool satisfied = false;

  for (int i = 0; i < size; i++) {
    unsigned lit = lits[i];
    char val = values[lit];

    if (val > 0) {
      satisfied = true;
      break;  // Clause already satisfied
    } else if (val == 0) {
      num_unassigned++;
      unassigned_lit = lit;
      if (num_unassigned > 1) break;  // Not unit
    }
  }

  if (satisfied) return;

  if (num_unassigned == 0) {
    // CONFLICT: All literals false
    unsigned idx = atomicAdd(conflict_count, 1);
    conflict_buffer[idx] = clause_id;
  } else if (num_unassigned == 1) {
    // UNIT IMPLICATION: Must assign unassigned_lit
    unsigned idx = atomicAdd(unit_count, 1);
    unit_buffer[idx] = unassigned_lit;
  }
  // else: > 1 unassigned, nothing to do
}
```

### Kernel 2: Binary Clause Fast Path

```cuda
__global__ void propagate_binary_watches(
  unsigned lit,
  unsigned *binary_watch_lists,  // Separate list for binary clauses
  unsigned *binary_offsets,
  char *values,
  unsigned *unit_buffer,
  unsigned *conflict_buffer,
  unsigned *unit_count,
  unsigned *conflict_count
) {
  int idx = blockIdx.x * blockDim.x + threadIdx.x;

  unsigned not_lit = lit ^ 1;
  unsigned start = binary_offsets[not_lit];
  unsigned end = binary_offsets[not_lit + 1];
  unsigned num_binary = end - start;

  if (idx >= num_binary) return;

  // Binary clause: just two literals
  // One is 'not_lit' (false), check the other
  unsigned other_lit = binary_watch_lists[start + idx];
  char other_val = values[other_lit];

  if (other_val < 0) {
    // CONFLICT
    unsigned cidx = atomicAdd(conflict_count, 1);
    conflict_buffer[cidx] = other_lit;  // Store conflicting info
  } else if (other_val == 0) {
    // UNIT IMPLICATION
    unsigned uidx = atomicAdd(unit_count, 1);
    unit_buffer[uidx] = other_lit;
  }
  // else: other_val > 0 (satisfied), nothing to do
}
```

### CPU-GPU Integration

```c
// In src/propsearch.c - replace kissat_search_propagate

#ifdef KISSAT_USE_GPU
#include "gpu_propagation.h"

clause *kissat_search_propagate(kissat *solver) {
  clause *res = NULL;
  unsigned *propagate = solver->propagate;

  while (!res && propagate != END_ARRAY(solver->trail)) {
    unsigned lit = *propagate++;

    // GPU PROPAGATION
    gpu_result result = gpu_propagate_literal(solver->gpu_state, lit);

    if (result.has_conflict) {
      // Build conflict clause from GPU result
      res = construct_conflict_clause(solver, &result);
      break;
    }

    // Apply unit implications found by GPU
    for (int i = 0; i < result.num_units; i++) {
      unsigned unit_lit = result.units[i];
      kissat_fast_binary_assign(solver, ..., unit_lit, lit);
    }

    solver->propagate = propagate;
  }

  return res;
}
#endif
```

---

## F1.CNF-Specific Optimizations

### Optimize for Binary/Ternary Structure

F1.CNF is 41% binary, 59% ternary - perfect for GPU!

```cuda
// Separate kernels for different clause sizes
__global__ void propagate_binary_clauses(...) {
  // Super fast: just check one other literal
  // 661K clauses × 1 check each = 661K threads
}

__global__ void propagate_ternary_clauses(...) {
  // Fast: check two other literals
  // 948K clauses × 2 checks each = 1.9M threads
}

__global__ void propagate_large_clauses(...) {
  // Rare in f1.cnf (<1%)
}
```

**Launch strategy:**
```c
// Process binary clauses (fastest)
propagate_binary_clauses<<<661000/256, 256>>>(...);

// Process ternary clauses
propagate_ternary_clauses<<<948000/256, 256>>>(...);

// Sync and collect results
cudaDeviceSynchronize();
```

### Memory Layout for Coalescing

```c
// Structure of Arrays (SoA) for coalesced access
struct gpu_clauses {
  // All binary clauses together
  unsigned binary_lits[1322000];  // 661K × 2 lits = contiguous

  // All ternary clauses together
  unsigned ternary_lits[2844000]; // 948K × 3 lits = contiguous

  // Much better GPU memory access than Array of Structures!
};
```

---

## Implementation Plan

### Phase 1: Basic GPU Propagation (Week 1-2)

**Files to create:**
```
src/gpu_propagation.h    - Interface
src/gpu_propagation.cu   - CUDA implementation
src/gpu_transfer.c       - CPU-GPU data transfer
```

**What to implement:**
1. Upload clause database to GPU (one-time)
2. Upload current assignment before each propagation
3. GPU kernel evaluates all clauses in parallel
4. Download results (units + conflicts)
5. CPU applies assignments

**Compilation:**
```bash
# Modify build system
nvcc -O3 -arch=sm_75 -c src/gpu_propagation.cu -o build/gpu_propagation.o
gcc -O3 ... -DKISSAT_USE_GPU -c src/*.c
gcc -o build/kissat build/*.o build/gpu_propagation.o -lcudart -lm
```

### Phase 2: Optimize Transfers (Week 3)

**Problem:** CPU-GPU transfer is slow (~5-10ms per transfer)

**Solution:** Minimize transfers
```c
// Keep values on GPU permanently
char *gpu_values;  // Stays on GPU

// Update only changed values
void update_gpu_values_incremental(unsigned *changed_lits, int count) {
  // Copy only 'count' values (not all 935K)
  cudaMemcpy(gpu_values + changed_lits[0], ..., count * sizeof(char), ...);
}
```

**Optimization:** Batch multiple propagations
```c
// Instead of: GPU call per literal
// Do: Accumulate 10-100 literals, then one GPU call

while (trail_has_unpropagated) {
  // Collect next N literals
  unsigned batch[100];
  int batch_size = 0;
  while (batch_size < 100 && has_more) {
    batch[batch_size++] = trail[propagate++];
  }

  // One GPU call for entire batch
  gpu_propagate_batch(batch, batch_size);
}
```

### Phase 3: Advanced (Week 4+)

1. **Persistent GPU state** - Keep solver state on GPU between calls
2. **GPU-side assignment** - Apply units on GPU without CPU roundtrip
3. **Conflict analysis on GPU** - Analyze conflicts without downloading
4. **Streams** - Overlap computation and transfer

---

## Expected Performance

### Breakdown for f1.cnf

**Current (CPU-only):**
- Propagation: ~140s (80% of time)
- Decision: ~20s (12%)
- Conflict analysis: ~10s (6%)
- Other: ~3s (2%)

**With GPU propagation:**
- Propagation: ~140s / 8 = **17.5s** (8x GPU speedup)
- Decision: ~20s (unchanged)
- Conflict analysis: ~10s (unchanged)
- Transfer overhead: ~5s (new)
- **Total: ~52.5s vs 172.75s = 3.3x overall speedup**

### With Optimization
- Propagation: ~140s / 12 = **11.7s** (12x with batching)
- Transfer: ~2s (batched)
- **Total: ~43.7s = 4x overall speedup**

### Ultimate (Full GPU)
- Propagation + assignment on GPU: ~10s
- Conflict on GPU: ~3s
- **Total: ~35s = 5x overall speedup**

---

## Concrete Implementation

**Minimal working version (Week 1):**

### File 1: src/gpu_propagation.h
```c
#ifndef _gpu_propagation_h_INCLUDED
#define _gpu_propagation_h_INCLUDED

typedef struct {
  unsigned *units;
  unsigned *conflicts;
  unsigned num_units;
  unsigned num_conflicts;
  bool has_conflict;
} gpu_propagation_result;

void gpu_propagation_init(struct kissat *solver);
void gpu_propagation_cleanup(struct kissat *solver);
void gpu_upload_clauses(struct kissat *solver);
void gpu_upload_values(struct kissat *solver);

gpu_propagation_result gpu_propagate_literal(
  struct kissat *solver,
  unsigned lit
);

#endif
```

### File 2: src/gpu_propagation.cu
```cuda
#include <cuda_runtime.h>
#include <stdio.h>

// GPU memory
static char *d_values = NULL;
static unsigned *d_binary_watches = NULL;
static unsigned *d_binary_offsets = NULL;
static unsigned *d_ternary_lits = NULL;
static unsigned *d_ternary_offsets = NULL;
static unsigned *d_unit_buffer = NULL;
static unsigned *d_conflict_buffer = NULL;
static unsigned *d_unit_count = NULL;
static unsigned *d_conflict_count = NULL;

// Binary clause propagation kernel
__global__ void propagate_binary_kernel(
  unsigned lit,
  unsigned *binary_watches,
  unsigned *binary_offsets,
  char *values,
  unsigned *unit_buffer,
  unsigned *conflict_buffer,
  unsigned *unit_count,
  unsigned *conflict_count
) {
  int idx = blockIdx.x * blockDim.x + threadIdx.x;

  unsigned not_lit = lit ^ 1;
  unsigned start = binary_offsets[not_lit];
  unsigned end = binary_offsets[not_lit + 1];
  unsigned num_watches = end - start;

  if (idx >= num_watches) return;

  // Binary clause: NOT(lit) ∨ other_lit
  // NOT(lit) is false, check other_lit
  unsigned other_lit = binary_watches[start + idx];
  char other_val = values[other_lit];

  if (other_val == 0) {
    // Unit implication
    unsigned pos = atomicAdd(unit_count, 1);
    unit_buffer[pos] = other_lit;
  } else if (other_val < 0) {
    // Conflict
    unsigned pos = atomicAdd(conflict_count, 1);
    conflict_buffer[pos] = idx;  // Watch index for conflict reconstruction
  }
  // else: satisfied, nothing to do
}

// Ternary clause propagation kernel
__global__ void propagate_ternary_kernel(
  unsigned lit,
  unsigned *ternary_lits,      // Flat array [lit0, lit1, lit2, lit0, lit1, lit2, ...]
  unsigned *ternary_offsets,   // Which ternaries watch lit
  char *values,
  unsigned *unit_buffer,
  unsigned *conflict_buffer,
  unsigned *unit_count,
  unsigned *conflict_count
) {
  int idx = blockIdx.x * blockDim.x + threadIdx.x;

  unsigned not_lit = lit ^ 1;
  unsigned start = ternary_offsets[not_lit];
  unsigned end = ternary_offsets[not_lit + 1];
  unsigned num_watches = end - start;

  if (idx >= num_watches) return;

  // Load ternary clause (3 literals)
  unsigned clause_offset = (start + idx) * 3;
  unsigned lit0 = ternary_lits[clause_offset];
  unsigned lit1 = ternary_lits[clause_offset + 1];
  unsigned lit2 = ternary_lits[clause_offset + 2];

  // NOT(lit) is one of these (and is false)
  // Find the other two
  unsigned other1, other2;
  if (lit0 == not_lit) {
    other1 = lit1; other2 = lit2;
  } else if (lit1 == not_lit) {
    other1 = lit0; other2 = lit2;
  } else {
    other1 = lit0; other2 = lit1;
  }

  char val1 = values[other1];
  char val2 = values[other2];

  // Check clause status
  if (val1 > 0 || val2 > 0) {
    // Satisfied
    return;
  }

  if (val1 < 0 && val2 < 0) {
    // CONFLICT: All three literals false
    unsigned pos = atomicAdd(conflict_count, 1);
    conflict_buffer[pos] = start + idx;
    return;
  }

  if (val1 == 0 && val2 < 0) {
    // UNIT: other1 must be true
    unsigned pos = atomicAdd(unit_count, 1);
    unit_buffer[pos] = other1;
  } else if (val2 == 0 && val1 < 0) {
    // UNIT: other2 must be true
    unsigned pos = atomicAdd(unit_count, 1);
    unit_buffer[pos] = other2;
  }
  // else: both unassigned, not unit yet
}

// Host function
extern "C" {

void gpu_propagate_literal_impl(
  unsigned lit,
  char *h_values,           // CPU values array
  unsigned *h_units,        // Output: units found
  unsigned *h_conflicts,    // Output: conflicts found
  unsigned *h_unit_count,   // Output: number of units
  unsigned *h_conflict_count// Output: number of conflicts
) {
  // Upload current assignment
  cudaMemcpy(d_values, h_values, 935342, cudaMemcpyHostToDevice);

  // Reset counters
  unsigned zero = 0;
  cudaMemcpy(d_unit_count, &zero, sizeof(unsigned), cudaMemcpyHostToDevice);
  cudaMemcpy(d_conflict_count, &zero, sizeof(unsigned), cudaMemcpyHostToDevice);

  // Launch binary propagation
  unsigned not_lit = lit ^ 1;
  unsigned h_binary_start, h_binary_end;
  cudaMemcpy(&h_binary_start, &d_binary_offsets[not_lit], sizeof(unsigned), cudaMemcpyDeviceToHost);
  cudaMemcpy(&h_binary_end, &d_binary_offsets[not_lit + 1], sizeof(unsigned), cudaMemcpyDeviceToHost);
  unsigned num_binary = h_binary_end - h_binary_start;

  if (num_binary > 0) {
    int blocks = (num_binary + 255) / 256;
    propagate_binary_kernel<<<blocks, 256>>>(
      lit, d_binary_watches, d_binary_offsets, d_values,
      d_unit_buffer, d_conflict_buffer, d_unit_count, d_conflict_count
    );
  }

  // Launch ternary propagation
  unsigned h_ternary_start, h_ternary_end;
  cudaMemcpy(&h_ternary_start, &d_ternary_offsets[not_lit], sizeof(unsigned), cudaMemcpyDeviceToHost);
  cudaMemcpy(&h_ternary_end, &d_ternary_offsets[not_lit + 1], sizeof(unsigned), cudaMemcpyDeviceToHost);
  unsigned num_ternary = h_ternary_end - h_ternary_start;

  if (num_ternary > 0) {
    int blocks = (num_ternary + 255) / 256;
    propagate_ternary_kernel<<<blocks, 256>>>(
      lit, d_ternary_lits, d_ternary_offsets, d_values,
      d_unit_buffer, d_conflict_buffer, d_unit_count, d_conflict_count
    );
  }

  // Synchronize
  cudaDeviceSynchronize();

  // Download results
  cudaMemcpy(h_unit_count, d_unit_count, sizeof(unsigned), cudaMemcpyDeviceToHost);
  cudaMemcpy(h_conflict_count, d_conflict_count, sizeof(unsigned), cudaMemcpyDeviceToHost);

  if (*h_conflict_count > 0) {
    cudaMemcpy(h_conflicts, d_conflict_buffer,
               *h_conflict_count * sizeof(unsigned), cudaMemcpyDeviceToHost);
  }

  if (*h_unit_count > 0) {
    cudaMemcpy(h_units, d_unit_buffer,
               *h_unit_count * sizeof(unsigned), cudaMemcpyDeviceToHost);
  }
}

} // extern "C"
```

---

## Memory Requirements

### GPU Memory for F1.CNF

**One-time allocation (uploaded once):**
- Binary watches: 661K × 2 lits × 4 bytes = 5.3 MB
- Binary offsets: 935K × 4 bytes = 3.7 MB
- Ternary lits: 948K × 3 lits × 4 bytes = 11.4 MB
- Ternary offsets: 935K × 4 bytes = 3.7 MB
- **Subtotal: 24.1 MB**

**Per-propagation (updated each call):**
- Values: 935K × 1 byte = 0.9 MB
- Unit buffer: max 100K × 4 bytes = 0.4 MB
- Conflict buffer: max 10K × 4 bytes = 0.04 MB
- **Subtotal: 1.34 MB**

**Total GPU memory:** ~26 MB (trivial for modern GPUs with 8-24 GB)

---

## Performance Analysis

### GPU Compute Performance

**CUDA cores available:** ~3000-10000 (depending on GPU)

**For binary propagation:**
- 661K clauses to check
- Launch 661K threads
- Each thread: 1 value lookup + 1 comparison = ~10 GPU cycles
- **Total time:661K × 10 / 3000 cores = ~2200 cycles = ~0.5 microseconds**

**For ternary propagation:**
- 948K clauses
- Each thread: 2 value lookups + comparison = ~20 cycles
- **Total time: ~1 microsecond**

**Per propagation compute: ~1.5 microseconds**

**vs CPU (current): ~1/8.7M = 115 nanoseconds**

**Wait, GPU is SLOWER?**

### The Transfer Bottleneck

**The problem:** Transfer overhead

**Upload values:** 935KB × ~5 GB/s = ~0.19 ms = **190 microseconds**
**Download results:** ~1KB × ~5 GB/s = ~0.2 microseconds
**GPU compute:** ~1.5 microseconds
**Total per call:** ~192 microseconds

**vs CPU:** ~115 nanoseconds

**GPU is 1660x SLOWER per propagation!**

---

## Why Naive GPU Fails

The transfer  overhead kills performance:
- Need to upload values before each propagation (190 μs)
- CPU propagates faster (0.115 μs)
- 1.5B propagations × 190 μs = **285,000 seconds = 79 hours!**

**Naive GPU would be 275x SLOWER than CPU!**

---

## How to Make GPU Actually Work

### Strategy A: Batch Propagation ✅ CRITICAL

**Don't call GPU per literal - batch 1000+ propagations:**

```c
// Accumulate trail
unsigned batch[10000];
int batch_size = 0;

while (propagating && batch_size < 10000) {
  batch[batch_size++] = trail[propagate++];
}

// ONE GPU call for 10,000 propagations
gpu_propagate_batch(batch, batch_size);

// Amortize transfer: 190μs / 10,000 = 0.019μs per propagation
// Now competitive with CPU!
```

**Batching analysis:**
- Batch of 1,000: 190μs / 1000 = 0.19μs (still slower)
- Batch of 10,000: 190μs / 10,000 = 0.019μs (**6x faster than CPU!**)
- Batch of 100,000: 190μs / 100,000 = 0.0019μs (**60x faster!**)

### Strategy B: Keep Values on GPU ✅ ESSENTIAL

```c
// Never download values - keep on GPU
char *d_values;  // Persistent GPU allocation

// When CPU makes assignment:
unsigned new_assignments[1000];
int count = collect_new_assignments(new_assignments, 1000);

// Upload only changed values (100 bytes, not 935KB!)
cudaMemcpy(d_values + new_assignments[0], &cpu_values[new_assignments[0]],
           count, cudaMemcpyHostToDevice);

// Now transfer is ~0.02 microseconds per update
```

### Strategy C: GPU-Side Trail ✅ ELIMINATE TRANSFERS

**Ultimate optimization:** Keep entire trail on GPU

```c
// All on GPU
__device__ char d_values[935342];
__device__ unsigned d_trail[1000000];
__device__ unsigned d_trail_size;

// GPU propagates without CPU involvement
__global__ void propagate_trail_kernel() {
 while (d_propagate < d_trail_size) {
    unsigned lit = d_trail[d_propagate++];
    propagate_literal_gpu(lit);  // All on GPU
  }
}

// CPU only:
// - Makes decisions
// - Analyzes conflicts (downloads conflict clause)
// - Manages search
```

**Now transfer:** Only conflicts (rare) and decisions (occasional)

---

## Optimized Architecture

### The Winning Design

**CPU (Master):**
1. Make decision (pick variable)
2. Upload decision to GPU
3. **GPU runs until conflict or completion**
4. GPU reports back: conflict or SAT
5. CPU analyzes conflict, learns clause
6. Repeat

**GPU (Worker):**
1. Receive decision from CPU
2. Assign literal to trail
3. **BCP loop entirely on GPU:**
   - While unpropagated literals exist
   - Propagate each literal (parallel)
   - Collect units (parallel)
   - Assign units to trail
   - Repeat
4. If conflict: report to CPU
5. If complete: report to CPU

**Communication:** ~1 transfer per conflict (1.8M conflicts / 1.5B props = 0.12%)

### Expected Performance

**With full GPU BCP:**
- CPU→GPU: 1.8M decisions × 1μs = 1.8 seconds
- GPU BCP: 1.5B props × 0.01μs = 15 seconds (100x faster than CPU!)
- GPU→CPU: 1.8M conflicts × 10μs = 18 seconds
- CPU analysis: 10 seconds
- **Total: ~45 seconds vs 172.75s = 3.8x speedup**

---

## Implementation Roadmap

### Week  1: Basic Infrastructure
**Files:**
- `src/gpu_propagation.h` - Interface
- `src/gpu_propagation.cu` - CUDA kernels
- `src/gpu_init.c` - Initialization/cleanup
- `Makefile` update for nvcc

**Deliverable:**
- Can upload/download data
- Basic kernel runs
- Test on small instance

### Week 2: Binary/Ternary Kernels
**Tasks:**
- Implement binary_kernel
- Implement ternary_kernel
- Integrate with kissat_search_propagate
- Test correctness

**Deliverable:**
- GPU propagation works (even if slow)
- Passes test suite

### Week 3: Optimize Transfers
**Tasks:**
- Remove per-literal transfers
- Implement batching (10K batch size)
- Keep values persistent on GPU
- Incremental value updates

**Deliverable:**
- 2m52s → ~90s (2x speedup)

### Week 4: Full GPU BCP Loop
**Tasks:**
- Move trail to GPU
- GPU-side assignment
- GPU-side unit detection
- GPU-side BCP loop

**Deliverable:**
- 2m52s → ~45s (4x speedup)

### Week 5-6: Advanced Optimizations
**Tasks:**
- Coalesced memory access
- Shared memory for watch lists
- Warp-level primitives
- Structure exploitation

**Deliverable:**
- 2m52s → ~25-35s (5-7x speedup)

---

## Code  Example: Minimal Integration

### Modify src/propsearch.c

```c
#include "propsearch.h"
#include "fastassign.h"
#include "print.h"
#include "trail.h"

#ifdef KISSAT_USE_GPU
#include "gpu_propagation.h"
#endif

clause *kissat_search_propagate (kissat *solver) {
  assert (!solver->probing);
  assert (solver->watching);
  assert (!solver->inconsistent);

  START (propagate);

  const unsigned *const saved_propagate = solver->propagate;

#ifdef KISSAT_USE_GPU
  // GPU-accelerated propagation
  clause *res = gpu_search_propagate(solver);
#else
  // Original CPU propagation
  clause *res = search_propagate(solver);
#endif

  update_search_propagation_statistics (solver, saved_propagate);

  STOP (propagate);

  return res;
}
```

---

## Build System Changes

### Makefile for CUDA

```makefile
# Detect nvcc
NVCC := $(shell which nvcc)

ifdef NVCC
  # CUDA available
  CFLAGS += -DKISSAT_USE_GPU
  GPU_OBJS = gpu_propagation.o gpu_init.o
  LDFLAGS += -L/usr/local/cuda/lib64 -lcudart

  %.o: %.cu
	$(NVCC) -O3 -arch=sm_75 -c $< -o $@
else
  # No CUDA
  GPU_OBJS =
endif

kissat: $(OBJS) $(GPU_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ -lm
```

### configure Script Update

```bash
# Detect CUDA
if which nvcc > /dev/null 2>&1; then
  echo "CUDA found: enabling GPU acceleration"
  CFLAGS="$CFLAGS -DKISSAT_USE_GPU"
  NVCC="nvcc"
else
  echo "CUDA not found: CPU-only build"
  NVCC=""
fi
```

---

## Testing Strategy

### Phase 1: Correctness
```bash
# Small instance
./kissat_gpu /tmp/test3.cnf
diff <(./kissat_cpu /tmp/test3.cnf) <(./kissat_gpu /tmp/test3.cnf)

# Medium instance
./kissat_gpu /tmp/test100.cnf

# Check results match
```

### Phase 2: Performance
```bash
# Measure GPU-only time
nvprof ./kissat_gpu f1.cnf

# Look for:
# - Kernel time
# - Transfer time
# - CPU time
# - Identify bottlenecks
```

### Phase 3: Optimization
```bash
# Profile with NVIDIA Nsight
nsys profile ./kissat_gpu f1.cnf

# Optimize:
# - Reduce transfers
# - Increase batch size
# - Coalesce memory access
# - Use shared memory
```

---

## Risk Analysis

### Technical Risks

**High Risk:**
- GPU may be slower if transfers dominate (mitigate with batching)
- Debugging CUDA is harder than CPU code
- Memory bugs cause silent corruption

**Medium Risk:**
- Results may differ due to floating point or race conditions
- GPU memory limits (unlikely with 26MB needed)
- Driver/CUDA version compatibility

**Low Risk:**
- Compiler issues (nvcc well supported)
- Build system complexity

### Mitigation

1. **Validate aggressively:** Compare every result with CPU version
2. **Start simple:** Get basic version working first
3. **Incremental:** Add complexity only when basics work
4. **Fallback:** Always keep CPU path as backup

---

## Expected Timeline & Results

### Minimum Viable Product (2 weeks)
- Basic GPU propagation working
- Correctness validated
- **Performance: 2m52s → ~90s (2x speedup)**

### Optimized Version (4 weeks)
- Batched transfers
- Full GPU BCP loop
- **Performance: 2m52s → ~45s (4x speedup)**

### Advanced (6 weeks)
- Structure exploitation
- Full optimization
- **Performance: 2m52s → ~25-35s (5-7x speedup)**

---

## Next Steps

1. **Check CUDA availability:**
   ```bash
   which nvcc
   nvidia-smi
   ```

2. **Implement basic kernel** (Day 1-2)
3. **Test on small instance** (Day 3)
4. **Optimize transfers** (Week 2)
5. **Full BCP on GPU** (Week 3-4)

Want me to start implementing the basic GPU propagation infrastructure?
