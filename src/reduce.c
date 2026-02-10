#include "reduce.h"
#include "allocate.h"
#include "collect.h"
#include "inline.h"
#include "kimits.h"
#include "print.h"
#include "rank.h"
#include "report.h"
#include "resources.h"
#include "tiers.h"
#include "trail.h"

#include <inttypes.h>
#include <math.h>

bool kissat_reducing (kissat *solver) {
  if (!GET_OPTION (reduce))
    return false;
  if (!solver->statistics.clauses_redundant)
    return false;
  if (CONFLICTS < solver->limits.reduce.conflicts)
    return false;
  return true;
}

typedef struct reducible reducible;

struct reducible {
  uint64_t rank;
  unsigned ref;
};

#define RANK_REDUCIBLE(RED) (RED).rank

// clang-format off
typedef STACK (reducible) reducibles;
// clang-format on

static bool collect_reducibles (kissat *solver, reducibles *reds,
                                reference start_ref) {
  assert (start_ref != INVALID_REF);
  assert (start_ref <= SIZE_STACK (solver->arena));
  ward *const arena = BEGIN_STACK (solver->arena);
  clause *start = (clause *) (arena + start_ref);
  const clause *const end = (clause *) END_STACK (solver->arena);
  assert (start < end);
  while (start != end && !start->redundant)
    start = kissat_next_clause (start);
  if (start == end) {
    solver->first_reducible = INVALID_REF;
    LOG ("no reducible clause candidate left");
    return false;
  }
  const reference redundant = (ward *) start - arena;
#ifdef LOGGING
  if (redundant < solver->first_reducible)
    LOG ("updating start of redundant clauses from %zu to %zu",
         (size_t) solver->first_reducible, (size_t) redundant);
  else
    LOG ("no update to start of redundant clauses %zu",
         (size_t) solver->first_reducible);
#endif
  solver->first_reducible = redundant;
  const unsigned tier1 = TIER1;
  const unsigned tier2 = MAX (tier1, TIER2);
  assert (tier1 <= tier2);
  for (clause *c = start; c != end; c = kissat_next_clause (c)) {
    if (!c->redundant)
      continue;
    if (c->garbage)
      continue;
    const unsigned used = c->used;
    if (used)
      c->used = used - 1;
    if (c->reason)
      continue;
    const unsigned glue = c->glue;
    if (glue <= tier1 && used)
      continue;
    if (glue <= tier2 && used >= MAX_USED - 1)
      continue;
    assert (kissat_clause_in_arena (solver, c));
    reducible red;
    const uint64_t negative_size = ~c->size;
    const uint64_t negative_glue = ~c->glue;
    red.rank = negative_size | (negative_glue << 32);
    red.ref = (ward *) c - arena;
    PUSH_STACK (*reds, red);
  }
  if (EMPTY_STACK (*reds)) {
    kissat_phase (solver, "reduce", GET (reductions),
                  "did not find any reducible redundant clause");
    return false;
  }
  return true;
}

#define USEFULNESS RANK_REDUCIBLE

static void sort_reducibles (kissat *solver, reducibles *reds) {
  RADIX_STACK (reducible, uint64_t, *reds, USEFULNESS);
}

static void mark_less_useful_clauses_as_garbage (kissat *solver,
                                                 reducibles *reds) {
  statistics *statistics = &solver->statistics;
  const double high = GET_OPTION (reducehigh) * 0.1;
  const double low = GET_OPTION (reducelow) * 0.1;
  double percent;
  if (low < high) {
    const double delta = high - low;
    percent = high - delta / log10 (statistics->reductions + 9);
  } else
    percent = low;
  const double fraction = percent / 100.0;
  const size_t size = SIZE_STACK (*reds);
  size_t target = size * fraction;
#ifndef QUIET
  const size_t clauses =
      statistics->clauses_irredundant + statistics->clauses_redundant;
  kissat_phase (solver, "reduce", GET (reductions),
                "reducing %zu (%.0f%%) out of %zu (%.0f%%) "
                "reducible clauses",
                target, kissat_percent (target, size), size,
                kissat_percent (size, clauses));
#endif
  unsigned reduced = 0, reduced1 = 0, reduced2 = 0, reduced3 = 0;
  ward *arena = BEGIN_STACK (solver->arena);
  const reducible *const begin = BEGIN_STACK (*reds);
  const reducible *const end = END_STACK (*reds);
  const unsigned tier1 = TIER1;
  const unsigned tier2 = TIER2;
  for (const reducible *p = begin; p != end && target--; p++) {
    clause *c = (clause *) (arena + p->ref);
    assert (kissat_clause_in_arena (solver, c));
    assert (!c->garbage);
    assert (!c->reason);
    assert (c->redundant);
    LOGCLS (c, "reducing");
    kissat_mark_clause_as_garbage (solver, c);
    reduced++;
    if (c->glue <= tier1)
      reduced1++;
    else if (c->glue <= tier2)
      reduced2++;
    else
      reduced3++;
  }
  ADD (clauses_reduced_tier1, reduced1);
  ADD (clauses_reduced_tier2, reduced2);
  ADD (clauses_reduced_tier3, reduced3);
  ADD (clauses_reduced, reduced);
}

/*
 * Calculate adaptive reduce interval based on search efficiency.
 * 
 * Strategy:
 * 1. If reduce takes too much time (>15% of total), increase interval
 * 2. If arena has lots of garbage (>25%), decrease interval  
 * 3. If arena is clean (<10%), can wait longer
 * 4. Smooth transitions using exponential moving average
 */
static uint64_t adaptive_reduce_delta (kissat *solver) {
  uint64_t base_delta = GET_OPTION (reduceint);
  double scale = 1.0;
  
  if (!GET_OPTION (reduceadaptive)) {
    // Standard Kissat behavior: use fixed scale (1.0)
    scale = solver->last.reduce_timing.current_scale;
  } else {
    // Adaptive mode: calculate dynamic scale based on overhead
    double prev_duration = solver->last.reduce_timing.duration;
    uint64_t prev_start_conflicts = solver->last.reduce_timing.prev_start_conflicts;
    double current_scale = solver->last.reduce_timing.current_scale;
    
    uint64_t conflicts_between = CONFLICTS - prev_start_conflicts;
    
    if (prev_duration > 0.0 && conflicts_between > 100) {
      double search_time = solver->last.reduce_timing.start_time - solver->last.reduce_timing.end_time;
      // Handle clock precision issues - ensure positive minimum
      if (search_time <= 0.0 || search_time < 0.001) search_time = 0.001;
      
      double overhead = prev_duration / (search_time + prev_duration);
      
      double target_scale = 1.0;
      
      // More conservative scaling for hard instances
      // High overhead on hard instances doesn't mean we should wait longer
      // Instead, we need to find a balance
      if (overhead > 0.25) {
        target_scale = 1.15;  // Was 1.30 - too aggressive
      } else if (overhead > 0.18) {
        target_scale = 1.08;  // Was 1.15
      } else if (overhead > 0.12) {
        target_scale = 1.03;  // Was 1.05
      } else if (overhead < 0.02) {
        target_scale = 0.95;  // Was 0.90 - don't reduce as much
      } else if (overhead < 0.04) {
        target_scale = 0.98;  // Was 0.95
      }
      
      double factor = GET_OPTION (reducefactor) / 100.0;
      target_scale = 1.0 + (target_scale - 1.0) * factor;
      
      // More smoothing for stability (was 75/25, now 85/15)
      double new_scale = current_scale * 0.85 + target_scale * 0.15;
      
      // Tighter bounds - don't let scale drift too far from 1.0
      if (new_scale < 0.7) new_scale = 0.7;   // Was 0.5
      if (new_scale > 1.5) new_scale = 1.5;   // Was 3.0 - major issue for hard instances
      
      solver->last.reduce_timing.current_scale = new_scale;
      
#ifndef QUIET
      kissat_phase (solver, "reduce", GET (reductions),
                    "adaptive: scale=%.2f overhead=%.1f%% -> next_int=%.0f",
                    new_scale, overhead * 100, base_delta * new_scale);
#endif
    }
    
    scale = solver->last.reduce_timing.current_scale;
  }
  
  // Calculate final delta with scaling
  uint64_t delta = (uint64_t)(base_delta * scale);
  
  // Apply sqrt scaling based on number of reductions (standard Kissat behavior)
  uint64_t reductions = solver->statistics.reductions;
  if (reductions > 0) {
    delta = (uint64_t)(delta * kissat_sqrt(reductions));
  }
  
  if (delta < 100) delta = 100;
  
  return delta;
}

int kissat_reduce (kissat *solver) {
  // Record start time for this reduction
  solver->last.reduce_timing.prev_start_conflicts = solver->last.reduce_timing.start_conflicts;
  solver->last.reduce_timing.start_conflicts = CONFLICTS;
  solver->last.reduce_timing.start_time = kissat_process_time ();
  
  START (reduce);
  INC (reductions);
  kissat_phase (solver, "reduce", GET (reductions),
                "reduce limit %" PRIu64 " hit after %" PRIu64 " conflicts",
                solver->limits.reduce.conflicts, CONFLICTS);
  kissat_compute_and_set_tier_limits (solver);
  bool compact = kissat_compacting (solver);
  reference start = compact ? 0 : solver->first_reducible;
  if (start != INVALID_REF) {
#ifndef QUIET
    size_t arena_size = SIZE_STACK (solver->arena);
    size_t words_to_sweep = arena_size - start;
    size_t bytes_to_sweep = sizeof (word) * words_to_sweep;
    kissat_phase (solver, "reduce", GET (reductions),
                  "reducing clauses after offset %" REFERENCE_FORMAT
                  " in arena",
                  start);
    kissat_phase (solver, "reduce", GET (reductions),
                  "reducing %zu words %s %.0f%%", words_to_sweep,
                  FORMAT_BYTES (bytes_to_sweep),
                  kissat_percent (words_to_sweep, arena_size));
#endif
    if (kissat_flush_and_mark_reason_clauses (solver, start)) {
      reducibles reds;
      INIT_STACK (reds);
      if (collect_reducibles (solver, &reds, start)) {
        sort_reducibles (solver, &reds);
        mark_less_useful_clauses_as_garbage (solver, &reds);
        RELEASE_STACK (reds);
        kissat_sparse_collect (solver, compact, start);
      } else if (compact)
        kissat_sparse_collect (solver, compact, start);
      else
        kissat_unmark_reason_clauses (solver, start);
    } else
      assert (solver->inconsistent);
  } else
    kissat_phase (solver, "reduce", GET (reductions), "nothing to reduce");
  kissat_classify (solver);
  
  // Calculate duration of this reduction
  double end_time = kissat_process_time ();
  solver->last.reduce_timing.end_time = end_time;
  solver->last.reduce_timing.duration = end_time - solver->last.reduce_timing.start_time;
  
  // Use adaptive reduce interval calculation
  uint64_t delta = adaptive_reduce_delta (solver);
  uint64_t new_limit = CONFLICTS + delta;
  solver->limits.reduce.conflicts = new_limit;
  
#ifndef QUIET
  kissat_phase (solver, "reduce", GET (reductions),
                "next reduce limit at %" PRIu64 " after %" PRIu64 " conflicts",
                new_limit, delta);
#endif
  
  solver->last.conflicts.reduce = CONFLICTS;
  REPORT (0, '-');
  STOP (reduce);
  return solver->inconsistent ? 20 : 0;
}
