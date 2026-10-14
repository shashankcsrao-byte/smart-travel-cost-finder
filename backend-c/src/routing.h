/*
 * routing.h - the graph algorithms, written by hand.
 *
 *   dijkstra / astar  one shared search with a binary min-heap and lazy deletion
 *   k_shortest_paths  Yen's algorithm: the k best loopless routes for one mode
 *   mixed_route       Dijkstra on (place, mode) states: journeys that change mode
 *   compare_routes    price every mode and rank the journeys by priority
 */
#ifndef ROUTING_H
#define ROUTING_H

#include <stddef.h>

#include "graph.h"

/* A cost is a pair compared element by element: the measure we minimise first,
   the other one as the tie-break (equal distance -> the quicker route wins). */
typedef struct {
    double a, b;
} Cost;
int cost_cmp(Cost x, Cost y);

enum { PRI_CHEAPEST, PRI_BALANCED, PRI_FASTEST, NPRI };
extern const char *const PRIORITY_NAMES[NPRI];
extern const double MINUTE_VALUE[NPRI];      /* rupees a minute is worth: 0, 2, 50 */
int priority_from_name(const char *name);    /* -1 if unknown */
#define TRANSFER_MIN 5.0                     /* walking + waiting when changing mode */

/* ---- replay trace: one event per settled node ---- */
typedef struct {
    int node;
    double t;
} NodeCost;

typedef struct {
    int node;
    double cost;
    NodeCost *relaxed;
    int nrelaxed, caprelaxed;
    NodeCost heap[4];            /* smallest live heap entries after this step */
    int nheap;
} TraceEvent;

typedef struct {
    TraceEvent *ev;
    int n, cap;
} Trace;

void trace_free(Trace *t);
TraceEvent *trace_add(Trace *t, int node, double cost);
void trace_relaxed(TraceEvent *e, int node, double t);

/* ---- single-mode search ---- */
typedef double (*Heuristic)(int node, void *ctx);

typedef struct {
    int *path;                   /* vertices from source to destination */
    int npath;
    const Edge **edges;          /* edges used, in order (npath - 1 of them) */
    int nedges;
    double km, min;
    int pops;                    /* nodes settled (to compare Dijkstra with A*) */
} PathResult;

void path_free(PathResult *p);

/* by_time = 0 minimises (distance, time); 1 minimises (time, distance). NULL if unreachable. */
PathResult *dijkstra(const Graph *g, int src, int dst, int mode, int by_time, Trace *trace);
PathResult *astar(const Graph *g, int src, int dst, int mode, Heuristic h, void *ctx, int by_time,
                  Trace *trace);
int k_shortest_paths(const Graph *g, int src, int dst, int mode, int k, PathResult **out);

/* ---- priced journeys ---- */
typedef struct {
    int mode;
    int *path;
    int npath;
    const Edge **edges;
    int nedges;
    double km, min, fare;
} MixLeg;

enum { VAR_CHEAPEST, VAR_FASTEST, VAR_MIXED };

typedef struct {
    int mode;                    /* -1 for a mixed journey */
    int variant;
    char label[192];
    int *path;
    int npath;
    const Edge **edges;
    int nedges;
    double km, min, fare;
    MixLeg *legs;                /* mixed journeys only: one entry per mode used */
    int nlegs;
} Journey;

void journey_id(const Journey *j, char *out, size_t n);   /* "bus", "bus_fastest" or "mixed" */
void journey_clear(Journey *j);
int journey_rank_cmp(const Journey *x, const Journey *y, int priority);

Journey *mixed_route(const Graph *g, int src, int dst, double minute_value, double transfer_min);

typedef struct {
    int src, dst, priority;
    Journey *opts;               /* ranked by priority */
    int nopts;
    int unavailable[NMODES];     /* modes with no route */
    int nunavailable;
} Comparison;

void compare_routes(const Graph *g, int src, int dst, int priority, int include_extras, Comparison *out);
void comparison_free(Comparison *c);
int comparison_cheapest(const Comparison *c);    /* index into opts, or -1 */
int comparison_fastest(const Comparison *c);

#endif
