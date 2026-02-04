#include "restart.h"
#include "backtrack.h"
#include "bump.h"
#include "decide.h"
#include "internal.h"
#include "kimits.h"
#include "logging.h"
#include "print.h"
#include "reluctant.h"
#include "report.h"

#include <inttypes.h>

bool kissat_restarting (kissat *solver) {
  assert (solver->unassigned);
  if (!GET_OPTION (restart))
    return false;
  if (!solver->level)
    return false;
  if (CONFLICTS < solver->limits.restart.conflicts)
    return false;
  if (solver->stable)
    return kissat_reluctant_triggered (&solver->reluctant);
  const double fast = AVERAGE (fast_glue);
  const double slow = AVERAGE (slow_glue);
  const double margin = (100.0 + GET_OPTION (restartmargin)) / 100.0;
  const double limit = margin * slow;
  kissat_extremely_verbose (solver,
                            "restart glue limit %g = "
                            "%.02f * %g (slow glue) %c %g (fast glue)",
                            limit, margin, slow,
                            (limit > fast    ? '>'
                             : limit == fast ? '='
                                             : '<'),
                            fast);
  return (limit <= fast);
}

/*
 * Adaptive restart interval calculation for Tseitin-encoded problems.
 * 
 * Strategy:
 * 1. High glue (LBD) indicates complex subproblems -> longer runs
 * 2. High vivification success means inprocessing is effective -> longer runs
 * 3. Low conflict rate relative to decisions -> longer runs
 */
static uint64_t adaptive_restart_delta (kissat *solver) {
  uint64_t base_delta = GET_OPTION (restartint);
  
  // If adaptive restarts disabled, use base logic
  if (!GET_OPTION (restartadaptive)) {
    uint64_t restarts = solver->statistics.restarts;
    uint64_t delta = base_delta;
    if (restarts)
      delta += kissat_logn (restarts) - 1;
    return delta;
  }
  
  uint64_t restarts = solver->statistics.restarts;
  
  // Base interval with log scaling
  uint64_t delta = base_delta;
  if (restarts)
    delta += kissat_logn (restarts) - 1;
  
  // Factor 1: Glue-based adaptation (higher glue = longer runs)
  const double fast = AVERAGE (fast_glue);
  const double slow = AVERAGE (slow_glue);
  double glue_factor = 1.0;
  
  if (slow > 0) {
    // If fast glue is close to slow glue, we're in a stable region
    double ratio = fast / slow;
    if (ratio > 0.9 && ratio < 1.1) {
      // Stable region - can run longer
      glue_factor = 1.5;
    } else if (fast > slow * 1.2) {
      // Fast glue much higher - complex subproblem, extend run
      glue_factor = 2.0;
    } else if (fast < slow * 0.8) {
      // Fast glue much lower - easy region, can restart more often
      glue_factor = 0.8;
    }
  }
  
  // Factor 2: Vivification effectiveness
  // High vivification means inprocessing is working well - let it continue
  double vivify_factor = 1.0;
  uint64_t vivified = solver->statistics.vivified;
  if (vivified > 10000 && CONFLICTS > 100000) {
    // Check vivification rate (per 1000 conflicts)
    double vivify_rate = (double) vivified / (CONFLICTS / 1000.0);
    if (vivify_rate > 5.0) {
      // High vivification rate - extend runs
      vivify_factor = 1.4;
    } else if (vivify_rate > 10.0) {
      // Very high vivification - significantly extend
      vivify_factor = 1.8;
    }
  }
  
  // Factor 3: Decision/conflict ratio
  // High ratio means we're learning well - can restart less often
  double decision_factor = 1.0;
  if (solver->statistics.decisions > 0 && CONFLICTS > 10000) {
    double dec_per_conflict = (double) solver->statistics.decisions / CONFLICTS;
    if (dec_per_conflict > 3.0) {
      // Many decisions per conflict - learning is effective
      decision_factor = 1.3;
    } else if (dec_per_conflict < 1.5) {
      // Few decisions per conflict - might need more restarts
      decision_factor = 0.9;
    }
  }
  
  // Combine factors
  double combined_factor = glue_factor * vivify_factor * decision_factor;
  
  // Clamp to reasonable bounds (0.5x to 3x of base)
  if (combined_factor < 0.5) combined_factor = 0.5;
  if (combined_factor > 3.0) combined_factor = 3.0;
  
  uint64_t adaptive_delta = (uint64_t) (delta * combined_factor);
  
  // Ensure minimum of 5 conflicts between restarts
  if (adaptive_delta < 5) adaptive_delta = 5;
  
  kissat_extremely_verbose (solver,
                            "adaptive restart factors: glue=%.2f vivify=%.2f decision=%.2f "
                            "combined=%.2f base_delta=%" PRIu64 " adaptive_delta=%" PRIu64,
                            glue_factor, vivify_factor, decision_factor,
                            combined_factor, delta, adaptive_delta);
  
  return adaptive_delta;
}

void kissat_update_focused_restart_limit (kissat *solver) {
  assert (!solver->stable);
  limits *limits = &solver->limits;
  uint64_t delta = adaptive_restart_delta (solver);
  limits->restart.conflicts = CONFLICTS + delta;
  kissat_extremely_verbose (solver,
                            "focused restart limit at %" PRIu64
                            " after %" PRIu64 " conflicts (adaptive)",
                            limits->restart.conflicts, delta);
}

static unsigned reuse_stable_trail (kissat *solver) {
  const heap *const scores = SCORES;
  const unsigned next_idx = kissat_next_decision_variable (solver);
  const double limit = kissat_get_heap_score (scores, next_idx);
  unsigned level = solver->level, res = 0;
  while (res < level) {
    frame *f = &FRAME (res + 1);
    const unsigned idx = IDX (f->decision);
    const double score = kissat_get_heap_score (scores, idx);
    if (score <= limit)
      break;
    res++;
  }
  return res;
}

static unsigned reuse_focused_trail (kissat *solver) {
  const links *const links = solver->links;
  const unsigned next_idx = kissat_next_decision_variable (solver);
  const unsigned limit = links[next_idx].stamp;
  LOG ("next decision variable stamp %u", limit);
  unsigned level = solver->level, res = 0;
  while (res < level) {
    frame *f = &FRAME (res + 1);
    const unsigned idx = IDX (f->decision);
    const unsigned score = links[idx].stamp;
    if (score <= limit)
      break;
    res++;
  }
  return res;
}

static unsigned reuse_trail (kissat *solver) {
  assert (solver->level);
  assert (!EMPTY_STACK (solver->trail));

  if (!GET_OPTION (restartreusetrail))
    return 0;

  unsigned res;

  if (solver->stable)
    res = reuse_stable_trail (solver);
  else
    res = reuse_focused_trail (solver);

  LOG ("matching trail level %u", res);

  if (res) {
    INC (restarts_reused_trails);
    ADD (restarts_reused_levels, res);
    LOG ("restart reuses trail at decision level %u", res);
  } else
    LOG ("restarts does not reuse the trail");

  return res;
}

void kissat_restart (kissat *solver) {
  START (restart);
  INC (restarts);
  ADD (restarts_levels, solver->level);
  if (solver->stable)
    INC (stable_restarts);
  else
    INC (focused_restarts);
  unsigned level = reuse_trail (solver);
  kissat_extremely_verbose (solver,
                            "restarting after %" PRIu64 " conflicts"
                            " (limit %" PRIu64 ")",
                            CONFLICTS, solver->limits.restart.conflicts);
  LOG ("restarting to level %u", level);
  kissat_backtrack_in_consistent_state (solver, level);
  if (!solver->stable)
    kissat_update_focused_restart_limit (solver);
  REPORT (1, 'R');
  STOP (restart);
}
