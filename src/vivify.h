#ifndef _vivify_h_INCLUDED
#define _vivify_h_INCLUDED

struct kissat;
struct clause;

void kissat_vivify (struct kissat *);
void kissat_bump_clause_vivify_activity (struct kissat *, struct clause *);

#endif
