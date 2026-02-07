#ifndef _binindex_h_INCLUDED
#define _binindex_h_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declaration
struct kissat;
typedef struct kissat kissat;

// Binary Implication Index
// Flat array storage for binary clause implications
// Provides O(1) access to all implications for a literal

typedef struct bin_impl_entry {
  unsigned lit;           // The implied literal
} bin_impl_entry;

typedef struct bin_impl_list {
  bin_impl_entry *entries;    // Flat array of implied literals
  unsigned count;             // Number of entries
  unsigned capacity;          // Allocated capacity
} bin_impl_list;

// Initialize the binary implication index
void kissat_init_bin_index (kissat *solver);

// Release memory for the binary implication index
void kissat_release_bin_index (kissat *solver);

// Full rebuild of the index from watch lists
void kissat_rebuild_bin_index (kissat *solver);

// Add a binary implication (a => b)
void kissat_bin_impl_add (kissat *solver, unsigned a, unsigned b);

// Remove a binary implication (a => b)
void kissat_bin_impl_remove (kissat *solver, unsigned a, unsigned b);

// Get all implications for a literal
// Returns pointer to entries and sets count
static inline const bin_impl_entry *kissat_get_bin_impl (kissat *solver, 
                                                          unsigned lit, 
                                                          unsigned *count) {
  extern bin_impl_list *kissat_get_bin_list (struct kissat *solver, unsigned lit);
  bin_impl_list *list = kissat_get_bin_list (solver, lit);
  if (list) {
    *count = list->count;
    return list->entries;
  }
  *count = 0;
  return NULL;
}

// Check if implication exists (linear search)
bool kissat_bin_impl_contains (kissat *solver, unsigned lit, unsigned other);

// Get count of implications for a literal
unsigned kissat_bin_impl_count (kissat *solver, unsigned lit);

// Propagate binary clauses using the implication index
// Note: Include proper headers before using (value.h, assign.h)
struct clause;
struct assigned;
struct clause *kissat_propagate_binary_index (
    kissat *solver, const unsigned not_lit, void *values,
    struct assigned *assigned, const bool probing, const unsigned level,
    uint64_t *ticks);

#endif // _binindex_h_INCLUDED
