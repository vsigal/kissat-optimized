#include "binindex.h"
#include "internal.h"
#include "watch.h"
#include "logging.h"
#include "allocate.h"
#include "vector.h"
#include "inlinevector.h"
#include "assign.h"
#include "value.h"
#include "inline.h"
#include "fastassign.h"

#include <assert.h>
#include <stddef.h>

// Binary Implication Index Implementation
// Provides fast O(1) access to binary clause implications

// Propagate binary clauses using the implication index
// Returns conflict clause if found, NULL otherwise
clause *kissat_propagate_binary_index (
    kissat *solver, const unsigned not_lit, void *values_void,
    assigned *assigned, const bool probing, const unsigned level,
    uint64_t *ticks) {
  
  // Cast values to proper type
  value *values = (value *)values_void;
  
  if (!solver->bin_index)
    return NULL;
  
  bin_impl_list *list = &solver->bin_index[not_lit];
  if (list->count == 0)
    return NULL;
  
  clause *conflict = NULL;
  const bin_impl_entry *entries = list->entries;
  const unsigned count = list->count;
  
  for (unsigned i = 0; i < count; i++) {
    const unsigned implied = entries[i].lit;
    const value implied_value = values[implied];
    
    if (implied_value > 0)
      continue;
    
    if (implied_value < 0) {
      // Conflict found
      conflict = kissat_binary_conflict (solver, not_lit, implied);
      return conflict;
    } else {
      // Unassigned - assign implied literal
      kissat_fast_binary_assign (solver, probing, level, values, assigned,
                                 implied, not_lit);
      (*ticks)++;
    }
  }
  
  return conflict;
}

// Get the bin_impl_list for a literal (external API)
bin_impl_list *kissat_get_bin_list (kissat *solver, unsigned lit) {
  if (!solver->bin_index)
    return NULL;
  assert (lit < LITS);
  return &solver->bin_index[lit];
}

void kissat_init_bin_index (kissat *solver) {
  LOG ("initializing binary implication index");
  assert (!solver->bin_index);
  
  const unsigned lits = LITS;
  solver->bin_index = kissat_calloc (solver, lits, sizeof (bin_impl_list));
  LOG ("allocated binary implication index for %u literals", lits);
}

void kissat_release_bin_index (kissat *solver) {
  if (!solver->bin_index)
    return;
    
  LOG ("releasing binary implication index");
  
  const unsigned lits = LITS;
  for (unsigned lit = 0; lit < lits; lit++) {
    bin_impl_list *list = &solver->bin_index[lit];
    if (list->entries) {
      kissat_free (solver, list->entries, list->capacity * sizeof (bin_impl_entry));
    }
  }
  
  kissat_free (solver, solver->bin_index, lits * sizeof (bin_impl_list));
  solver->bin_index = NULL;
  LOG ("released binary implication index");
}

// First pass: count binary clauses per literal
static void count_binary_clauses (kissat *solver, unsigned *counts) {
  const unsigned lits = LITS;
  
  for (unsigned lit = 0; lit < lits; lit++) {
    counts[lit] = 0;
  }
  
  // Safety check: ensure watches array is allocated
  if (!solver->watches)
    return;
  
  for (unsigned lit = 0; lit < lits; lit++) {
    watches ws = solver->watches[lit];
    
    // Skip empty watch lists
    if (EMPTY_WATCHES (ws))
      continue;
    
    watch *begin = BEGIN_WATCHES (ws);
    watch *end = END_WATCHES (ws);
    
    // Safety check: ensure pointers are valid
    if (!begin || !end)
      continue;
    
    for (watch *p = begin; p != end; p++) {
      watch w = *p;
      if (w.type.binary) {
        // Count this binary clause (lit => blocking)
        // Binary watches have only ONE element (no tail)
        counts[lit]++;
      } else {
        // Large watches have TWO elements (head + tail)
        p++; // Skip tail for large clause
      }
    }
  }
}

// Second pass: allocate and populate entries
static void populate_bin_index (kissat *solver, unsigned *counts, unsigned **positions) {
  const unsigned lits = LITS;
  
  // Allocate entries for each literal
  for (unsigned lit = 0; lit < lits; lit++) {
    bin_impl_list *list = &solver->bin_index[lit];
    list->count = 0;
    list->capacity = counts[lit];
    if (list->capacity > 0) {
      list->entries = kissat_malloc (solver, list->capacity * sizeof (bin_impl_entry));
    } else {
      list->entries = NULL;
    }
    positions[lit] = (unsigned *)list->entries; // Use as cursor during population
  }
  
  // Safety check: ensure watches array is allocated
  if (!solver->watches)
    return;
  
  // Populate entries from watch lists
  for (unsigned lit = 0; lit < lits; lit++) {
    watches ws = solver->watches[lit];
    
    // Skip empty watch lists
    if (EMPTY_WATCHES (ws))
      continue;
    
    watch *begin = BEGIN_WATCHES (ws);
    watch *end = END_WATCHES (ws);
    
    // Safety check: ensure pointers are valid
    if (!begin || !end)
      continue;
    
    for (watch *p = begin; p != end; p++) {
      watch w = *p;
      if (w.type.binary) {
        // Binary watches have only ONE element (no tail)
        unsigned blocking = w.blocking.lit;
        
        bin_impl_list *list = &solver->bin_index[lit];
        if (list->entries) {
          unsigned idx = list->count++;
          list->entries[idx].lit = blocking;
        }
      } else {
        // Large watches have TWO elements (head + tail)
        p++; // Skip tail for large clause
      }
    }
  }
}

void kissat_rebuild_bin_index (kissat *solver) {
  LOG ("rebuilding binary implication index");
  
  if (!solver->bin_index) {
    kissat_init_bin_index (solver);
  }
  
  const unsigned lits = LITS;
  
  // Release old entries
  for (unsigned lit = 0; lit < lits; lit++) {
    bin_impl_list *list = &solver->bin_index[lit];
    if (list->entries) {
      kissat_free (solver, list->entries, list->capacity * sizeof (bin_impl_entry));
      list->entries = NULL;
    }
    list->count = 0;
    list->capacity = 0;
  }
  
  // Count binary clauses
  unsigned *counts = kissat_malloc (solver, lits * sizeof (unsigned));
  count_binary_clauses (solver, counts);
  
  unsigned total = 0;
  for (unsigned lit = 0; lit < lits; lit++) {
    total += counts[lit];
  }
  LOG ("found %u total binary implications", total);
  
  // Allocate and populate
  unsigned **positions = kissat_malloc (solver, lits * sizeof (unsigned *));
  populate_bin_index (solver, counts, positions);
  
  kissat_free (solver, counts, lits * sizeof (unsigned));
  kissat_free (solver, positions, lits * sizeof (unsigned *));
  
  LOG ("rebuilt binary implication index");
}

void kissat_bin_impl_add (kissat *solver, unsigned a, unsigned b) {
  if (!solver->bin_index)
    return;
  
  assert (a < LITS);
  bin_impl_list *list = &solver->bin_index[a];
  
  // Check if already exists
  for (unsigned i = 0; i < list->count; i++) {
    if (list->entries[i].lit == b)
      return; // Already exists
  }
  
  // Resize if needed
  if (list->count >= list->capacity) {
    unsigned new_capacity = list->capacity ? list->capacity * 2 : 4;
    bin_impl_entry *new_entries = kissat_malloc (solver, new_capacity * sizeof (bin_impl_entry));
    if (list->entries) {
      for (unsigned i = 0; i < list->count; i++) {
        new_entries[i] = list->entries[i];
      }
      kissat_free (solver, list->entries, list->capacity * sizeof (bin_impl_entry));
    }
    list->entries = new_entries;
    list->capacity = new_capacity;
  }
  
  // Add entry
  list->entries[list->count++].lit = b;
  LOG ("added binary implication %s => %s", LOGLIT (a), LOGLIT (b));
}

void kissat_bin_impl_remove (kissat *solver, unsigned a, unsigned b) {
  if (!solver->bin_index)
    return;
  
  assert (a < LITS);
  bin_impl_list *list = &solver->bin_index[a];
  
  // Find and remove entry
  for (unsigned i = 0; i < list->count; i++) {
    if (list->entries[i].lit == b) {
      // Shift remaining entries
      for (unsigned j = i; j < list->count - 1; j++) {
        list->entries[j] = list->entries[j + 1];
      }
      list->count--;
      LOG ("removed binary implication %s => %s", LOGLIT (a), LOGLIT (b));
      return;
    }
  }
}

bool kissat_bin_impl_contains (kissat *solver, unsigned lit, unsigned other) {
  if (!solver->bin_index)
    return false;
  
  assert (lit < LITS);
  bin_impl_list *list = &solver->bin_index[lit];
  
  for (unsigned i = 0; i < list->count; i++) {
    if (list->entries[i].lit == other)
      return true;
  }
  return false;
}

unsigned kissat_bin_impl_count (kissat *solver, unsigned lit) {
  if (!solver->bin_index)
    return 0;
  
  assert (lit < LITS);
  return solver->bin_index[lit].count;
}
