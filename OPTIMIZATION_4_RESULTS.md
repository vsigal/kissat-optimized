# Optimization #4: Conflict Batching - Results

## Status: ⚠️ Reverted

### Attempt
Implemented conflict batching with:
- Batch size of 4 conflicts
- Delayed bumping of analyzed variables
- Single-pass batch bump for better cache locality

### Implementation Details
Added to `src/analyze.c`:
- `conflict_batch_full()` - Check if batch is full
- `conflict_batch_add()` - Add conflict to batch
- `conflict_batch_process()` - Process full batch
- `batch_bump_analyzed()` - Batch bump all analyzed variables

### Results
- **Before (Opt #2 only)**: ~148-149s
- **With Opt #4**: ~150-152s
- **Difference**: +2-3s (1-2% slower)

### Why It Didn't Work
1. **Conflict analysis is sequential** - Each conflict depends on the previous one (learned clauses)
2. **Batching adds complexity** - Need to track batch state, check fullness, process deferred bumps
3. **Cache benefits not realized** - The analyzed[] array is already cache-friendly for single conflicts
4. **Overhead exceeds benefits** - Branch mispredictions from batch checking outweigh cache gains

### Lessons Learned
1. **Not all batching improves performance** - Sequential dependencies limit batching effectiveness
2. **Cache locality is already good** - The existing bump code is well-optimized
3. **Simplicity wins** - Adding state machines adds overhead

### Final State
- ✅ Optimization #2 (Size Specialization) working: ~148-149s
- ❌ Optimization #4 (Conflict Batching) reverted
- ✅ Binary in ./build/ ready for testing

### Files
- Binary: `./build/kissat` (Optimization #2 only)
- Modified: `src/proplit.h`, `src/simdscan.c`
