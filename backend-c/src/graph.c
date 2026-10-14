/*
 * graph.c - see graph.h.
 */
#include "graph.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

const char *const MODE_NAMES[NMODES] = {"bus", "metro", "bike_taxi", "car_taxi"};

int mode_from_name(const char *name) {
    for (int m = 0; m < NMODES; m++)
        if (strcmp(MODE_NAMES[m], name) == 0) return m;
    return -1;
}

Graph *graph_new(const char *const *names, int n) {
    Graph *g = xcalloc(1, sizeof *g);
    g->n = n;
    g->names = xcalloc((size_t)n, sizeof *g->names);
    g->adj = xcalloc((size_t)n, sizeof *g->adj);
    g->deg = xcalloc((size_t)n, sizeof *g->deg);
    g->cap = xcalloc((size_t)n, sizeof *g->cap);
    g->index = sm_new((size_t)n);
    for (int i = 0; i < n; i++) {
        g->names[i] = xstrdup(names[i]);
        sm_put(g->index, names[i], (void *)(intptr_t)(i + 1));
    }
    return g;
}

void graph_free(Graph *g) {
    if (!g) return;
    for (int i = 0; i < g->n; i++) {
        free(g->names[i]);
        free(g->adj[i]);
    }
    free(g->names);
    free(g->adj);
    free(g->deg);
    free(g->cap);
    sm_free(g->index, NULL);
    free(g);
}

int graph_add_edge(Graph *g, int from, int to, int mode, double km, double min) {
    if (from < 0 || to < 0 || from >= g->n || to >= g->n) return -1;
    if (km < 0 || min < 0) return -1;      /* Dijkstra is only correct without negative weights */
    if (g->deg[from] == g->cap[from]) {
        g->cap[from] = g->cap[from] ? g->cap[from] * 2 : 4;
        g->adj[from] = xrealloc(g->adj[from], (size_t)g->cap[from] * sizeof(Edge));
    }
    g->adj[from][g->deg[from]++] = (Edge){to, mode, km, min};
    return 0;
}

int graph_index(const Graph *g, const char *name) {
    intptr_t v = (intptr_t)sm_get(g->index, name);
    return v ? (int)v - 1 : -1;
}

/* ------------------------------------------------------------ sample city */

const char *const DEMO_LOCATIONS[DEMO_NLOC] = {
    "College", "Residential Area", "Market", "Bus Stand", "Metro Station", "Railway Station",
    "Hospital", "City Centre", "Shopping Mall", "Tech Park", "Junction", "Airport",
};

/* Hand-placed (x, y) positions, used only for drawing the network map. */
const double DEMO_COORDS[DEMO_NLOC][2] = {
    {0.0, 2.3}, {0.5, 0.2}, {1.6, 3.1}, {2.8, 2.2}, {1.2, 4.6}, {2.8, 5.5},
    {3.9, 0.7}, {4.3, 3.3}, {5.3, 4.6}, {6.7, 3.6}, {5.7, 1.5}, {8.2, 2.3},
};

/* Each line is ONE two-way connection: (place a, place b, mode, km, minutes). */
const Connection DEMO_CONNECTIONS[DEMO_NCONN] = {
    /* ---------------- Bus (14) ---------------- */
    {"College", "Market", MODE_BUS, 3, 12},
    {"College", "Residential Area", MODE_BUS, 4, 15},
    {"Market", "Bus Stand", MODE_BUS, 2, 8},
    {"Market", "Metro Station", MODE_BUS, 2, 8},
    {"Residential Area", "Bus Stand", MODE_BUS, 5, 18},
    {"Bus Stand", "Railway Station", MODE_BUS, 3, 12},
    {"Bus Stand", "Hospital", MODE_BUS, 4, 15},
    {"Bus Stand", "City Centre", MODE_BUS, 4, 16},
    {"Hospital", "Junction", MODE_BUS, 5, 18},
    {"City Centre", "Shopping Mall", MODE_BUS, 3, 12},
    {"City Centre", "Junction", MODE_BUS, 6, 22},
    {"Shopping Mall", "Tech Park", MODE_BUS, 4, 15},
    {"Junction", "Airport", MODE_BUS, 12, 35},
    {"Tech Park", "Airport", MODE_BUS, 14, 40},
    /* ---------------- Metro (6) ---------------- */
    {"College", "Metro Station", MODE_METRO, 2, 5},
    {"Metro Station", "City Centre", MODE_METRO, 6, 10},
    {"Metro Station", "Railway Station", MODE_METRO, 4, 7},
    {"Railway Station", "City Centre", MODE_METRO, 3, 5},
    {"City Centre", "Tech Park", MODE_METRO, 7, 12},
    {"Tech Park", "Airport", MODE_METRO, 12, 18},
    /* ---------------- Bike taxi (13) ---------------- */
    {"College", "Market", MODE_BIKE_TAXI, 3, 7},
    {"College", "Residential Area", MODE_BIKE_TAXI, 4, 9},
    {"Market", "Bus Stand", MODE_BIKE_TAXI, 2, 5},
    {"Market", "Metro Station", MODE_BIKE_TAXI, 2, 5},
    {"Market", "City Centre", MODE_BIKE_TAXI, 5, 11},
    {"Bus Stand", "Railway Station", MODE_BIKE_TAXI, 3, 7},
    {"Bus Stand", "Hospital", MODE_BIKE_TAXI, 4, 9},
    {"Hospital", "Junction", MODE_BIKE_TAXI, 5, 11},
    {"City Centre", "Junction", MODE_BIKE_TAXI, 5, 10},
    {"City Centre", "Shopping Mall", MODE_BIKE_TAXI, 3, 7},
    {"City Centre", "Tech Park", MODE_BIKE_TAXI, 7, 15},
    {"Junction", "Airport", MODE_BIKE_TAXI, 11, 22},
    {"Tech Park", "Airport", MODE_BIKE_TAXI, 13, 25},
    /* ---------------- Car taxi (14) ---------------- */
    {"College", "Market", MODE_CAR_TAXI, 3, 8},
    {"College", "Residential Area", MODE_CAR_TAXI, 4, 10},
    {"Residential Area", "Tech Park", MODE_CAR_TAXI, 9, 15},
    {"Market", "Bus Stand", MODE_CAR_TAXI, 2, 6},
    {"Market", "Metro Station", MODE_CAR_TAXI, 2, 6},
    {"Market", "City Centre", MODE_CAR_TAXI, 5, 14},
    {"Bus Stand", "Railway Station", MODE_CAR_TAXI, 3, 8},
    {"Bus Stand", "Hospital", MODE_CAR_TAXI, 4, 10},
    {"Hospital", "Junction", MODE_CAR_TAXI, 5, 12},
    {"City Centre", "Junction", MODE_CAR_TAXI, 5, 12},
    {"City Centre", "Shopping Mall", MODE_CAR_TAXI, 3, 8},
    {"Shopping Mall", "Tech Park", MODE_CAR_TAXI, 4, 10},
    {"Junction", "Airport", MODE_CAR_TAXI, 12, 20},
    {"Tech Park", "Airport", MODE_CAR_TAXI, 15, 18},
};

Graph *build_demo_graph(void) {
    Graph *g = graph_new(DEMO_LOCATIONS, DEMO_NLOC);
    for (int i = 0; i < DEMO_NCONN; i++) {
        const Connection *c = &DEMO_CONNECTIONS[i];
        int a = graph_index(g, c->a), b = graph_index(g, c->b);
        if (graph_add_edge(g, a, b, c->mode, c->km, c->min) || graph_add_edge(g, b, a, c->mode, c->km, c->min)) {
            fprintf(stderr, "fatal: bad connection %s - %s\n", c->a, c->b);
            exit(1);
        }
    }
    return g;
}

int find_location(const Graph *g, const char *name) {
    char *wanted = collapse_spaces(name);
    int found = -1;
    for (int i = 0; i < g->n && found < 0; i++)
        if (str_ieq(g->names[i], wanted)) found = i;
    free(wanted);
    return found;
}
