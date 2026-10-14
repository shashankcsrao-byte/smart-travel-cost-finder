/*
 * graph.h - the city as a weighted graph, stored as an ADJACENCY LIST.
 *
 * Vertices are places (numbered 0..n-1, each with a name). Every vertex has an
 * array of the edges leaving it; an edge is one direct link for ONE transport
 * mode and carries a distance (km) and a time (minutes).
 *
 *     adj[College] -> [ {to: Market, bus, 3 km, 12 min}, ... ]
 *
 * Memory: O(V + E). Looping over a vertex's neighbours costs O(its degree).
 */
#ifndef GRAPH_H
#define GRAPH_H

#include "strmap.h"

enum { MODE_BUS, MODE_METRO, MODE_BIKE_TAXI, MODE_CAR_TAXI, NMODES };
extern const char *const MODE_NAMES[NMODES];        /* "bus", "metro", "bike_taxi", "car_taxi" */
int mode_from_name(const char *name);               /* -1 if unknown */

typedef struct {
    int to;
    int mode;
    double km;
    double min;
} Edge;

typedef struct {
    int n;
    char **names;
    Edge **adj;          /* adj[v] = edges leaving v */
    int *deg, *cap;
    StrMap *index;       /* name -> vertex number + 1 */
} Graph;

Graph *graph_new(const char *const *names, int n);
void graph_free(Graph *g);
int graph_add_edge(Graph *g, int from, int to, int mode, double km, double min);   /* -1 if invalid */
int graph_index(const Graph *g, const char *name);                                 /* -1 if unknown */

/* ---- the sample city (illustrative data, not real Bengaluru values) ---- */
#define DEMO_NLOC 12
#define DEMO_NCONN 47
typedef struct {
    const char *a, *b;
    int mode;
    double km, min;
} Connection;

extern const char *const DEMO_LOCATIONS[DEMO_NLOC];
extern const double DEMO_COORDS[DEMO_NLOC][2];
extern const Connection DEMO_CONNECTIONS[DEMO_NCONN];

Graph *build_demo_graph(void);        /* every connection becomes two directed edges */
int find_location(const Graph *g, const char *name);   /* case- and space-insensitive */

#endif
