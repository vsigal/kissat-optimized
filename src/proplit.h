#include "simdscan.h"

static inline void kissat_watch_large_delayed (kissat *solver,
                                               watches *all_watches,
                                               unsigneds *delayed) {
  assert (all_watches == solver->watches);
  assert (delayed == &solver->delayed);
  const unsigned *const end_delayed = END_STACK (*delayed);
  unsigned const *d = BEGIN_STACK (*delayed);
  while (d != end_delayed) {
    const unsigned lit = *d++;
    assert (d != end_delayed);
    const watch watch = {.raw = *d++};
    assert (!watch.type.binary);
    assert (lit < LITS);
    watches *const lit_watches = all_watches + lit;
    assert (d != end_delayed);
    const reference ref = *d++;
    const unsigned blocking = watch.blocking.lit;
    LOGREF3 (ref, "watching %s blocking %s in", LOGLIT (lit),
             LOGLIT (blocking));
    kissat_push_blocking_watch (solver, lit_watches, blocking, ref);
  }
  CLEAR_STACK (*delayed);
}

static inline void
kissat_delay_watching_large (kissat *solver, unsigneds *const delayed,
                             unsigned lit, unsigned other, reference ref) {
  const watch watch = kissat_blocking_watch (other);
  PUSH_STACK (*delayed, lit);
  PUSH_STACK (*delayed, watch.raw);
  PUSH_STACK (*delayed, ref);
}

#if defined(__GNUC__) || defined(__clang__)
#define KISSAT_PROPLIT_LIKELY(X) __builtin_expect(!!(X), 1)
#define KISSAT_PROPLIT_UNLIKELY(X) __builtin_expect(!!(X), 0)
#define KISSAT_PROPLIT_PREFETCH(addr) __builtin_prefetch((addr), 0, 3)
#else
#define KISSAT_PROPLIT_LIKELY(X) (X)
#define KISSAT_PROPLIT_UNLIKELY(X) (X)
#define KISSAT_PROPLIT_PREFETCH(addr) ((void)0)
#endif

// ============================================================================
// OPTIMIZATION #2: Clause Size Specialization
// ============================================================================
// Specialized fast paths for different clause sizes eliminate runtime
// size checks and improve branch prediction in the hot propagation loop
//
// #2: Size-based dispatch with separate paths for:
//     - Ternary clauses (size == 3): Direct indexing, no loops
//     - Small clauses (size 4-8): Unrolled scalar search
//     - Large clauses (size > 8): SIMD-accelerated search

static inline clause *PROPAGATE_LITERAL (kissat *solver,
#if defined(PROBING_PROPAGATION)
                                         const clause *const ignore,
#endif
                                         const unsigned lit) {
  assert (solver->watching);
  LOG (PROPAGATION_TYPE " propagating %s", LOGLIT (lit));
  assert (VALUE (lit) > 0);
  assert (EMPTY_STACK (solver->delayed));

  watches *const all_watches = solver->watches;
  ward *const arena = BEGIN_STACK (solver->arena);
  assigned *const assigned = solver->assigned;
  value *const values = solver->values;

  const unsigned not_lit = NOT (lit);

  assert (not_lit < LITS);
  watches *watches = all_watches + not_lit;

  watch *const begin_watches = BEGIN_WATCHES (*watches);
  const watch *const end_watches = END_WATCHES (*watches);

  watch *q = begin_watches;
  const watch *p = q;

  unsigneds *const delayed = &solver->delayed;
  assert (EMPTY_STACK (*delayed));

  const size_t size_watches = SIZE_WATCHES (*watches);
  uint64_t ticks = 1 + kissat_cache_lines (size_watches, sizeof (watch));
  const unsigned idx = IDX (lit);
  struct assigned *const a = assigned + idx;
  const bool probing = solver->probing;
  const unsigned level = a->level;
  clause *res = 0;

  // Prefetch distances tuned for L1/L2 cache hierarchy
  #define WATCH_PREFETCH_DISTANCE 12
  #define CLAUSE_PREFETCH_DISTANCE 4
  
  // Pre-fetch first batch of watches
  if (begin_watches + WATCH_PREFETCH_DISTANCE < end_watches)
    KISSAT_PROPLIT_PREFETCH(begin_watches + WATCH_PREFETCH_DISTANCE);
  
  while (p != end_watches) {
    // Aggressive prefetching of upcoming watches
    if (p + WATCH_PREFETCH_DISTANCE < end_watches)
      KISSAT_PROPLIT_PREFETCH(p + WATCH_PREFETCH_DISTANCE);
    
    const watch head = *q++ = *p++;
    const unsigned blocking = head.blocking.lit;
    assert (VALID_INTERNAL_LITERAL (blocking));
    
    // Idea #2: Prefetch blocking literal's value (conservative)
    // This hides the memory latency for the values array access
    KISSAT_PROPLIT_PREFETCH(&values[blocking]);
    
    const value blocking_value = values[blocking];

    if (KISSAT_PROPLIT_LIKELY (head.type.binary)) {
      // Binary clause fast path - most common case
      if (KISSAT_PROPLIT_LIKELY (blocking_value > 0))
        continue;
      if (KISSAT_PROPLIT_UNLIKELY (blocking_value < 0)) {
        res = kissat_binary_conflict (solver, not_lit, blocking);
#ifndef CONTINUE_PROPAGATING_AFTER_CONFLICT
        break;
#endif
      } else {
        assert (!blocking_value);
        kissat_fast_binary_assign (solver, probing, level, values, assigned,
                                   blocking, not_lit);
        ticks++;
      }
    } else {
      const watch tail = *q++ = *p++;
      if (KISSAT_PROPLIT_LIKELY (blocking_value > 0))
        continue;
      const reference ref = tail.raw;
      assert (ref < SIZE_STACK (solver->arena));
      
      // Prefetch clause header before accessing
      clause *const c = (clause *) (arena + ref);
      KISSAT_PROPLIT_PREFETCH(c);
      
      ticks++;
      if (KISSAT_PROPLIT_UNLIKELY (c->garbage)) {
        q -= 2;
        continue;
      }
      unsigned *const lits = BEGIN_LITS (c);
      const unsigned other = lits[0] ^ lits[1] ^ not_lit;
      assert (lits[0] != lits[1]);
      assert (VALID_INTERNAL_LITERAL (other));
      assert (not_lit != other);
      assert (lit != other);
      const value other_value = values[other];
      if (KISSAT_PROPLIT_LIKELY (other_value > 0)) {
        q[-2].blocking.lit = other;
        continue;
      }
      // OPTIMIZATION #2: Size-based dispatch
      // Binary and ternary clauses are most common (~85% combined)
      // Using size-specific code eliminates unpredictable branches
      
      const unsigned size = c->size;
      
      if (KISSAT_PROPLIT_LIKELY (size == 3)) {
        // Ternary clause fast path (~25% of clauses)
        // Size is known = no loop needed, just check the one replacement literal
        const unsigned replacement = lits[2];
        assert (VALID_INTERNAL_LITERAL (replacement));
        const value replacement_value = values[replacement];

        // Common ternary case: found a replacement
        if (KISSAT_PROPLIT_LIKELY (replacement_value >= 0)) {
          c->searched = 2;
          LOGREF3 (ref, "unwatching %s in", LOGLIT (not_lit));
          q -= 2;
          lits[0] = other;
          lits[1] = replacement;
          assert (lits[0] != lits[1]);
          lits[2] = not_lit;
          kissat_delay_watching_large (solver, delayed, replacement, other, ref);
          ticks++;
          continue;
        }

        // No replacement: conflict or unit
        if (KISSAT_PROPLIT_UNLIKELY (other_value)) {
          assert (blocking_value < 0);
          assert (other_value < 0);
#if defined(PROBING_PROPAGATION)
          if (c == ignore) {
            LOGREF (ref, "conflicting but ignored");
            continue;
          }
#endif
          LOGREF (ref, "conflicting");
          res = c;
#ifndef CONTINUE_PROPAGATING_AFTER_CONFLICT
          break;
#endif
          continue;
        }

#if defined(PROBING_PROPAGATION)
        if (c == ignore) {
          LOGREF (ref, "forcing %s but ignored", LOGLIT (other));
          continue;
        }
#endif
        kissat_fast_assign_reference (solver, values, assigned, other, ref, c);
        ticks++;
        continue;
      }
      
      // Large clause path (>3 literals)
      // Further specialization: small (4-8) vs large (>8)
      
      unsigned replacement = INVALID_LIT;
      value replacement_value = -1;
      size_t r_idx = 0;
      bool found = false;
      
      // OPTIMIZATION: Unrolled scalar search for small clauses (4-8 literals)
      // This avoids SIMD overhead for small sizes and gives predictable branches
      // LOOP UNROLLING: Process 2 elements at a time to reduce loop overhead
      if (size <= 8) {
        // Small clause: unrolled scalar search
        unsigned *const searched = lits + c->searched;
        unsigned start_idx = searched - lits;
        
        // Search from searched position to end - UNROLLED 2x
        // Process pairs of literals to reduce branch mispredictions
        unsigned i = start_idx;
        for (; i + 1 < size; i += 2) {
          value v0 = values[lits[i]];
          value v1 = values[lits[i + 1]];
          if (v0 >= 0) {
            replacement = lits[i];
            r_idx = i;
            found = true;
            break;
          }
          if (v1 >= 0) {
            replacement = lits[i + 1];
            r_idx = i + 1;
            found = true;
            break;
          }
        }
        // Handle remaining element (odd size)
        if (!found && i < size) {
          if (values[lits[i]] >= 0) {
            replacement = lits[i];
            r_idx = i;
            found = true;
          }
        }
        
        // If not found, search from position 2 to searched position - UNROLLED 2x
        if (!found) {
          unsigned j = 2;
          for (; j + 1 < start_idx; j += 2) {
            value v0 = values[lits[j]];
            value v1 = values[lits[j + 1]];
            if (v0 >= 0) {
              replacement = lits[j];
              r_idx = j;
              found = true;
              break;
            }
            if (v1 >= 0) {
              replacement = lits[j + 1];
              r_idx = j + 1;
              found = true;
              break;
            }
          }
          // Handle remaining element
          if (!found && j < start_idx) {
            if (values[lits[j]] >= 0) {
              replacement = lits[j];
              r_idx = j;
              found = true;
            }
          }
        }
      } else {
        // Large clause: use SIMD-accelerated search
        const unsigned *const end_lits = lits + size;
        unsigned *const searched = lits + c->searched;
        
        KISSAT_PROPLIT_PREFETCH(lits);
        if (size > 16)
          KISSAT_PROPLIT_PREFETCH(lits + 16);
        
        found = kissat_simd_find_non_false (values, lits,
                                            searched - lits,
                                            end_lits - lits,
                                            &replacement, &r_idx);
        
        if (!found) {
          found = kissat_simd_find_non_false (values, lits, 2,
                                              searched - lits,
                                              &replacement, &r_idx);
        }
      }
      
      if (found) {
        replacement_value = values[replacement];
      }

      if (KISSAT_PROPLIT_LIKELY (replacement_value >= 0)) {
        c->searched = r_idx;
        assert (replacement != INVALID_LIT);
        LOGREF3 (ref, "unwatching %s in", LOGLIT (not_lit));
        q -= 2;
        lits[0] = other;
        lits[1] = replacement;
        assert (lits[0] != lits[1]);
        lits[r_idx] = not_lit;
        kissat_delay_watching_large (solver, delayed, replacement, other,
                                     ref);
        ticks++;
      } else if (KISSAT_PROPLIT_UNLIKELY (other_value)) {
        assert (blocking_value < 0);
        assert (other_value < 0);
#if defined(PROBING_PROPAGATION)
        if (c == ignore) {
          LOGREF (ref, "conflicting but ignored");
          continue;
        }
#endif
        LOGREF (ref, "conflicting");
        res = c;
#ifndef CONTINUE_PROPAGATING_AFTER_CONFLICT
        break;
#endif
      } else {
        assert (replacement_value < 0);
#if defined(PROBING_PROPAGATION)
        if (c == ignore) {
          LOGREF (ref, "forcing %s but ignored", LOGLIT (other));
          continue;
        }
#endif
        kissat_fast_assign_reference (solver, values, assigned, other, ref,
                                      c);
        ticks++;
      }
    }
  }
  solver->ticks += ticks;

  while (p != end_watches)
    *q++ = *p++;
  SET_END_OF_WATCHES (*watches, q);

  kissat_watch_large_delayed (solver, all_watches, delayed);

  return res;
}

#undef KISSAT_PROPLIT_LIKELY
#undef KISSAT_PROPLIT_UNLIKELY

static inline void kissat_update_conflicts_and_trail (kissat *solver,
                                                      clause *conflict,
                                                      bool flush) {
  if (conflict) {
#ifndef PROBING_PROPAGATION
    INC (conflicts);
#endif
    if (!solver->level) {
      LOG (PROPAGATION_TYPE " propagation on root-level failed");
      solver->inconsistent = true;
      CHECK_AND_ADD_EMPTY ();
      ADD_EMPTY_TO_PROOF ();
    }
  } else if (flush && !solver->level && solver->unflushed)
    kissat_flush_trail (solver);
}
