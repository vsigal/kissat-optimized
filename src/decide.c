#include "decide.h"
#include "inlineframes.h"
#include "inlineheap.h"
#include "inlinequeue.h"
#include "print.h"

#include <inttypes.h>

// Branch prediction hints
#if defined(__GNUC__) || defined(__clang__)
#define DECIDE_LIKELY(X) __builtin_expect(!!(X), 1)
#define DECIDE_UNLIKELY(X) __builtin_expect(!!(X), 0)
#else
#define DECIDE_LIKELY(X) (X)
#define DECIDE_UNLIKELY(X) (X)
#endif

/*
 * Tseitin-Aware Decision Heuristic
 * 
 * In Tseitin-encoded circuits, variables are created in layers:
 * - Level 0: Input variables (original problem inputs)
 * - Level 1: First-level gate outputs
 * - Level 2: Second-level gate outputs, etc.
 * 
 * Deciding lower-level variables first maximizes propagation
 * because they influence all higher-level variables.
 */

// Estimate Tseitin level from variable ID
// Returns 0 for likely inputs, higher for intermediate variables
static inline unsigned tseitin_level (kissat *solver, unsigned idx) {
  if (!GET_OPTION (tseitindec))
    return 0;
  
  // Simple heuristic: use log2 of index to estimate level
  // This assumes roughly exponential growth in variable count per level
  if (idx < 1000)
    return 0;
  
  // Estimate level based on magnitude of index
  // Variables with similar magnitudes are likely in the same layer
  unsigned level = 0;
  unsigned threshold = 1000;
  while (idx > threshold && level < 10) {
    level++;
    threshold *= 3;  // Assume roughly 3x growth per level
  }
  
  return level;
}

// Find best unassigned variable in queue, preferring lower Tseitin levels
// Always returns a valid unassigned variable (asserted)
static unsigned find_tseitin_preferred_variable (kissat *solver) {
  assert (solver->unassigned);
  const links *const links = solver->links;
  const value *const values = solver->values;
  
  // Start from current queue position
  unsigned start = solver->queue.search.idx;
  unsigned res = start;
  
  // If already unassigned, we're done (fast path)
  // Note: This is often true at the start of search, less so later
  if (DECIDE_LIKELY (!values[LIT (res)]))
    return res;
  
  // Search for best unassigned variable
  unsigned best = INVALID_IDX;
  unsigned best_level = UINT_MAX;
  unsigned first_unassigned = INVALID_IDX;
  unsigned candidate = res;
  
  // First pass: find the first unassigned (fallback) + best Tseitin level
  for (int i = 0; i < 1000 && !DISCONNECTED (candidate); i++) {
    unsigned lit = LIT (candidate);
    if (!values[lit]) {
      // Record first unassigned as fallback
      if (first_unassigned == INVALID_IDX)
        first_unassigned = candidate;
      
      // Check Tseitin level
      unsigned level = tseitin_level (solver, candidate);
      if (level < best_level) {
        best = candidate;
        best_level = level;
        if (level == 0)  // Can't do better than level 0
          break;
      }
    }
    candidate = links[candidate].prev;
  }
  
  // Determine result: prefer best Tseitin level, fallback to first unassigned
  if (best != INVALID_IDX) {
    res = best;
  } else if (first_unassigned != INVALID_IDX) {
    res = first_unassigned;
  } else {
    // Emergency fallback: scan forward from start to find any unassigned
    // This shouldn't happen but ensures we never return invalid
    candidate = start;
    for (int i = 0; i < 10000 && !DISCONNECTED (candidate); i++) {
      if (!values[LIT (candidate)]) {
        res = candidate;
        break;
      }
      candidate = links[candidate].next;
    }
    
    // Final emergency: scan all variables
    if (values[LIT (res)]) {
      for (unsigned idx = 0; idx < VARS; idx++) {
        if (ACTIVE (idx) && !values[LIT (idx)]) {
          res = idx;
          break;
        }
      }
    }
  }
  
  // CRITICAL: Must return an unassigned variable
  assert (!values[LIT (res)]);
  assert (!DISCONNECTED (res));
  
  // Update queue position
  kissat_update_queue (solver, links, res);
  
  return res;
}

static unsigned last_enqueued_unassigned_variable (kissat *solver) {
  assert (solver->unassigned);
  
  unsigned res;
  if (GET_OPTION (tseitindec)) {
    // Use Tseitin-aware selection
    res = find_tseitin_preferred_variable (solver);
  } else {
    // Original behavior
    const links *const links = solver->links;
    const value *const values = solver->values;
    res = solver->queue.search.idx;
    if (values[LIT (res)]) {
      do {
        res = links[res].prev;
        assert (!DISCONNECTED (res));
      } while (values[LIT (res)]);
      kissat_update_queue (solver, links, res);
    }
  }
  
#ifdef LOGGING
  const links *const links = solver->links;
  const unsigned stamp = links[res].stamp;
  unsigned level = GET_OPTION (tseitindec) ? tseitin_level (solver, res) : 0;
  LOG ("last enqueued unassigned %s stamp %u tseitin_level %u", 
       LOGVAR (res), stamp, level);
#endif
#ifdef CHECK_QUEUE
  const links *const links = solver->links;
  for (unsigned i = links[res].next; !DISCONNECTED (i); i = links[i].next)
    assert (VALUE (LIT (i)));
#endif
  return res;
}

// Forward declarations for decision cache
static void invalidate_decision_cache (kissat *solver);
static void fill_decision_cache (kissat *solver);

// Check if cached variable is still valid (unassigned and active)
static inline bool cache_entry_valid (kissat *solver, unsigned idx) {
  if (idx == INVALID_IDX)
    return false;
  if (!ACTIVE (idx))
    return false;
  return !solver->values[LIT (idx)];
}

// Try to get a variable from the decision cache
static unsigned get_from_decision_cache (kissat *solver) {
  if (!solver->decision_cache_valid)
    return INVALID_IDX;
  
  // Try to find a valid entry in the cache
  for (unsigned i = 0; i < solver->decision_cache_size; i++) {
    unsigned idx = solver->decision_cache[i];
    if (cache_entry_valid (solver, idx)) {
      // Move this entry to front (LRU) and return it
      if (i > 0) {
        for (unsigned j = i; j > 0; j--)
          solver->decision_cache[j] = solver->decision_cache[j-1];
        solver->decision_cache[0] = idx;
      }
      solver->decision_cache_hits++;
      return idx;
    }
  }
  
  // No valid entries found
  solver->decision_cache_valid = false;
  solver->decision_cache_misses++;
  return INVALID_IDX;
}

// Fill the decision cache with top candidates from the heap
static void fill_decision_cache (kissat *solver) {
  heap *scores = SCORES;
  const value *const values = solver->values;
  
  solver->decision_cache_size = 0;
  
  // Get up to DECISION_CACHE_SIZE candidates
  for (unsigned i = 0; i < DECISION_CACHE_SIZE * 2; i++) {
    if (kissat_empty_heap (scores))
      break;
    
    unsigned idx = kissat_max_heap (scores);
    
    // Skip assigned variables but don't pop them yet
    if (values[LIT (idx)]) {
      // Pop assigned variables
      kissat_pop_max_heap (solver, scores);
      continue;
    }
    
    // Found an unassigned variable
    solver->decision_cache[solver->decision_cache_size++] = idx;
    
    if (solver->decision_cache_size >= DECISION_CACHE_SIZE)
      break;
  }
  
  solver->decision_cache_valid = (solver->decision_cache_size > 0);
}

// Invalidate the decision cache (call when variables are assigned)
static void invalidate_decision_cache (kissat *solver) {
  solver->decision_cache_valid = false;
  solver->decision_cache_size = 0;
}

static unsigned largest_score_unassigned_variable (kissat *solver) {
  // Try decision cache first (Optimization #4)
  unsigned cached = get_from_decision_cache (solver);
  if (cached != INVALID_IDX) {
    LOG ("decision cache hit: %s", LOGVAR (cached));
    return cached;
  }
  
  // Cache miss - fill it and try again
  fill_decision_cache (solver);
  cached = get_from_decision_cache (solver);
  if (cached != INVALID_IDX) {
    LOG ("decision cache filled, returning: %s", LOGVAR (cached));
    return cached;
  }
  
  // Fallback to original implementation
  heap *scores = SCORES;
  unsigned res = kissat_max_heap (scores);
  const value *const values = solver->values;
  // Most variables in heap are assigned, so this loop typically runs multiple times
  while (DECIDE_LIKELY (values[LIT (res)])) {
    kissat_pop_max_heap (solver, scores);
    res = kissat_max_heap (scores);
  }
#if defined(LOGGING) || defined(CHECK_HEAP)
  const double score = kissat_get_heap_score (scores, res);
#endif
  LOG ("largest score unassigned %s score %g", LOGVAR (res), score);
#ifdef CHECK_HEAP
  for (all_variables (idx)) {
    if (!ACTIVE (idx))
      continue;
    if (VALUE (LIT (idx)))
      continue;
    const double idx_score = kissat_get_heap_score (scores, idx);
    assert (score >= idx_score);
  }
#endif
  return res;
}

void kissat_start_random_sequence (kissat *solver) {
  if (!GET_OPTION (randec))
    return;

  if (solver->stable && !GET_OPTION (randecstable))
    return;

  if (!solver->stable && !GET_OPTION (randecfocused))
    return;

  if (solver->randec)
    kissat_very_verbose (solver,
                         "continuing random decision sequence "
                         "at %s conflicts",
                         FORMAT_COUNT (CONFLICTS));
  else {
    INC (random_sequences);
    const uint64_t count = solver->statistics.random_sequences;
    const unsigned length = GET_OPTION (randeclength) * LOGN (count);
    kissat_very_verbose (solver,
                         "starting random decision sequence "
                         "at %s conflicts for %s conflicts",
                         FORMAT_COUNT (CONFLICTS), FORMAT_COUNT (length));
    solver->randec = length;

    UPDATE_CONFLICT_LIMIT (randec, random_sequences, LOGN, false);
  }
}

static unsigned next_random_decision (kissat *solver) {
  if (!VARS)
    return INVALID_IDX;

  if (solver->warming)
    return INVALID_IDX;

  if (!GET_OPTION (randec))
    return INVALID_IDX;

  if (solver->stable && !GET_OPTION (randecstable))
    return INVALID_IDX;

  if (!solver->stable && !GET_OPTION (randecfocused))
    return INVALID_IDX;

  if (!solver->randec) {
    assert (solver->level);
    if (solver->level > 1)
      return INVALID_IDX;

    uint64_t conflicts = CONFLICTS;
    limits *limits = &solver->limits;
    if (conflicts < limits->randec.conflicts)
      return INVALID_IDX;

    kissat_start_random_sequence (solver);
  }

  for (;;) {
    unsigned idx = kissat_next_random32 (&solver->random) % VARS;
    if (DECIDE_UNLIKELY (!ACTIVE (idx)))
      continue;
    unsigned lit = LIT (idx);
    if (DECIDE_UNLIKELY (solver->values[lit]))
      continue;
    return idx;
  }
}

unsigned kissat_next_decision_variable (kissat *solver) {
#ifdef LOGGING
  const char *type = 0;
#endif
  unsigned res = next_random_decision (solver);
  if (res == INVALID_IDX) {
    if (solver->stable) {
#ifdef LOGGING
      type = "maximum score";
#endif
      res = largest_score_unassigned_variable (solver);
      INC (score_decisions);
    } else {
#ifdef LOGGING
      type = "dequeued";
#endif
      res = last_enqueued_unassigned_variable (solver);
      INC (queue_decisions);
    }
  } else {
#ifdef LOGGING
    type = "random";
#endif
    INC (random_decisions);
  }
  LOG ("next %s decision %s", type, LOGVAR (res));
  return res;
}

int kissat_decide_phase (kissat *solver, unsigned idx) {
  bool force = GET_OPTION (forcephase);

  value *target;
  if (force)
    target = 0;
  else if (!GET_OPTION (target))
    target = 0;
  else if (solver->stable || GET_OPTION (target) > 1)
    target = solver->phases.target + idx;
  else
    target = 0;

  value *saved;
  if (force)
    saved = 0;
  else if (GET_OPTION (phasesaving))
    saved = solver->phases.saved + idx;
  else
    saved = 0;

  value res = 0;

  if (!solver->stable) {
    switch ((solver->statistics.switched >> 1) & 7) {
    case 1:
      res = INITIAL_PHASE;
      break;
    case 3:
      res = -INITIAL_PHASE;
      break;
    }
  }

  if (!res && target && (res = *target)) {
    LOG ("%s uses target decision phase %d", LOGVAR (idx), (int) res);
    INC (target_decisions);
  }

  if (!res && saved && (res = *saved)) {
    LOG ("%s uses saved decision phase %d", LOGVAR (idx), (int) res);
    INC (saved_decisions);
  }

  if (!res) {
    res = INITIAL_PHASE;
    LOG ("%s uses initial decision phase %d", LOGVAR (idx), (int) res);
    INC (initial_decisions);
  }
  assert (res);

  return res < 0 ? -1 : 1;
}

void kissat_decide (kissat *solver) {
  START (decide);
  assert (solver->unassigned);
  if (solver->warming)
    INC (warming_decisions);
  else {
    INC (decisions);
    if (solver->stable)
      INC (stable_decisions);
    else
      INC (focused_decisions);
  }
  solver->level++;
  assert (solver->level != INVALID_LEVEL);
  const unsigned idx = kissat_next_decision_variable (solver);
  const value value = kissat_decide_phase (solver, idx);
  unsigned lit = LIT (idx);
  if (value < 0)
    lit = NOT (lit);
  kissat_push_frame (solver, lit);
  assert (solver->level < SIZE_STACK (solver->frames));
  LOG ("decide literal %s", LOGLIT (lit));
  kissat_assign_decision (solver, lit);
  STOP (decide);
}

void kissat_internal_assume (kissat *solver, unsigned lit) {
  assert (solver->unassigned);
  assert (!VALUE (lit));
  solver->level++;
  assert (solver->level != INVALID_LEVEL);
  kissat_push_frame (solver, lit);
  assert (solver->level < SIZE_STACK (solver->frames));
  LOG ("assuming literal %s", LOGLIT (lit));
  kissat_assign_decision (solver, lit);
}
