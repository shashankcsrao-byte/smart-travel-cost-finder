/*
 * routing.c - see routing.h.
 */
#include "routing.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fares.h"
#include "heap.h"
#include "strmap.h"
#include "util.h"

const char *const PRIORITY_NAMES[NPRI] = {"cheapest", "balanced", "fastest"};
const double MINUTE_VALUE[NPRI] = {0.0, 2.0, 50.0};

int priority_from_name(const char *name) {
    for (int p = 0; p < NPRI; p++)
        if (strcmp(PRIORITY_NAMES[p], name) == 0) return p;
    return -1;
}

int cost_cmp(Cost x, Cost y) {
    if (x.a < y.a) return -1;
    if (x.a > y.a) return 1;
    if (x.b < y.b) return -1;
    if (x.b > y.b) return 1;
    return 0;
}

static const Cost COST_INF = {INFINITY, INFINITY};

/* ================================================================== traces */

void trace_free(Trace *t) {
    if (!t) return;
    for (int i = 0; i < t->n; i++) free(t->ev[i].relaxed);
    free(t->ev);
    t->ev = NULL;
    t->n = t->cap = 0;
}

TraceEvent *trace_add(Trace *t, int node, double cost) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 32;
        t->ev = xrealloc(t->ev, (size_t)t->cap * sizeof *t->ev);
    }
    TraceEvent *e = &t->ev[t->n++];
    memset(e, 0, sizeof *e);
    e->node = node;
    e->cost = cost;
    return e;
}

void trace_relaxed(TraceEvent *e, int node, double t) {
    if (e->nrelaxed == e->caprelaxed) {
        e->caprelaxed = e->caprelaxed ? e->caprelaxed * 2 : 4;
        e->relaxed = xrealloc(e->relaxed, (size_t)e->caprelaxed * sizeof *e->relaxed);
    }
    e->relaxed[e->nrelaxed++] = (NodeCost){node, t};
}

void path_free(PathResult *p) {
    if (!p) return;
    free(p->path);
    free(p->edges);
    free(p);
}

/* ============================================= Dijkstra and A* (one search) */

typedef struct {
    Cost prio;
    int node;
    const char *name;            /* ties are broken by name, exactly like Python's heap tuples */
} HItem;

static int hitem_cmp(const void *x, const void *y) {
    const HItem *a = x, *b = y;
    int c = cost_cmp(a->prio, b->prio);
    return c ? c : strcmp(a->name, b->name);
}

typedef struct {
    const Graph *g;
    Heuristic h;
    void *ctx;
} SearchCtx;

/* Dijkstra: priority = cost. A*: cost so far + estimate of the cost still to go. */
static Cost priority_of(const SearchCtx *s, int node, Cost cost) {
    return (Cost){cost.a + (s->h ? s->h(node, s->ctx) : 0.0), cost.b};
}

static int edge_banned(const Edge *e, const Edge *const *banned, int nbanned) {
    for (int i = 0; i < nbanned; i++)
        if (banned[i] == e) return 1;
    return 0;
}

static PathResult *search(const Graph *g, int src, int dst, int mode, int by_time, Trace *trace,
                          Heuristic h, void *hctx, const Edge *const *banned_edges, int nbanned,
                          const char *banned_nodes) {
    SearchCtx s = {g, h, hctx};
    int n = g->n;

    /* 1. Initialise: source = 0, every other place = infinity. */
    Cost *dist = xmalloc((size_t)n * sizeof *dist);
    int *prev = xmalloc((size_t)n * sizeof *prev);
    const Edge **prev_edge = xcalloc((size_t)n, sizeof *prev_edge);
    char *visited = xcalloc((size_t)n, 1);
    for (int i = 0; i < n; i++) {
        dist[i] = COST_INF;
        prev[i] = -1;
    }
    dist[src] = (Cost){0, 0};

    Heap heap;
    heap_init(&heap, sizeof(HItem), hitem_cmp);
    heap_push(&heap, &(HItem){priority_of(&s, src, dist[src]), src, g->names[src]});
    int pops = 0;
    HItem it;

    while (heap_pop(&heap, &it)) {
        /* 2. Take the unvisited place with the smallest priority. */
        int u = it.node;

        /* 3. Skip stale entries (a binary heap cannot "decrease a key"). */
        if (visited[u] || cost_cmp(it.prio, priority_of(&s, u, dist[u])) > 0) continue;
        visited[u] = 1;
        pops++;
        TraceEvent *ev = trace ? trace_add(trace, u, dist[u].a) : NULL;

        if (u == dst) break;                       /* final once popped -> stop early */

        /* 4. Relax every edge of the requested mode leaving u. */
        for (int i = 0; i < g->deg[u]; i++) {
            const Edge *e = &g->adj[u][i];
            if (e->mode != mode) continue;
            if ((banned_nodes && banned_nodes[e->to]) || edge_banned(e, banned_edges, nbanned)) continue;
            Cost step = by_time ? (Cost){e->min, e->km} : (Cost){e->km, e->min};
            Cost next = {dist[u].a + step.a, dist[u].b + step.b};
            if (cost_cmp(next, dist[e->to]) < 0) {
                dist[e->to] = next;
                prev[e->to] = u;
                prev_edge[e->to] = e;
                heap_push(&heap, &(HItem){priority_of(&s, e->to, next), e->to, g->names[e->to]});
                if (ev) trace_relaxed(ev, e->to, next.a);
            }
        }
        if (ev) {
            /* The heap after this step, smallest first, without stale entries (for the replay). */
            size_t count = heap.count;
            HItem *copy = xmalloc((count ? count : 1) * sizeof *copy);
            memcpy(copy, heap.data, count * sizeof *copy);
            qsort(copy, count, sizeof *copy, hitem_cmp);
            for (size_t k = 0; k < count && k < 12 && ev->nheap < 4; k++) {
                int v = copy[k].node;
                if (!visited[v] && cost_cmp(copy[k].prio, priority_of(&s, v, dist[v])) <= 0)
                    ev->heap[ev->nheap++] = (NodeCost){v, dist[v].a};
            }
            free(copy);
        }
    }
    heap_free(&heap);

    PathResult *result = NULL;
    if (!(isinf(dist[dst].a) && isinf(dist[dst].b))) {
        /* 5. Rebuild the path from the parent links, adding up the real totals. */
        int count = 1;
        for (int v = dst; v != src; v = prev[v]) count++;
        result = xcalloc(1, sizeof *result);
        result->npath = count;
        result->nedges = count - 1;
        result->path = xmalloc((size_t)count * sizeof(int));
        result->edges = xmalloc((size_t)(count > 1 ? count - 1 : 1) * sizeof(Edge *));
        int i = count - 1;
        for (int v = dst; v != src; v = prev[v], i--) {
            result->path[i] = v;
            result->edges[i - 1] = prev_edge[v];
        }
        result->path[0] = src;
        double km = 0, min = 0;
        for (int k = 0; k < result->nedges; k++) {
            km += result->edges[k]->km;
            min += result->edges[k]->min;
        }
        result->km = py_round(km, 3);
        result->min = py_round(min, 3);
        result->pops = pops;
    }
    free(dist);
    free(prev);
    free(prev_edge);
    free(visited);
    return result;
}

/* Lowest-cost route from src to dst using ONLY `mode` edges.
   Time: O((V + E) log V) with the binary heap.  Space: O(V + E). */
PathResult *dijkstra(const Graph *g, int src, int dst, int mode, int by_time, Trace *trace) {
    return search(g, src, dst, mode, by_time, trace, NULL, NULL, NULL, 0, NULL);
}

/* A*: the heuristic must never overestimate (admissible), otherwise the route can be worse. */
PathResult *astar(const Graph *g, int src, int dst, int mode, Heuristic h, void *ctx, int by_time,
                  Trace *trace) {
    return search(g, src, dst, mode, by_time, trace, h, ctx, NULL, 0, NULL);
}

/* ===================================================== Yen's k-shortest paths */

typedef struct {
    Cost cost;
    int tie;
    PathResult *route;
} Candidate;

static int candidate_cmp(const void *x, const void *y) {
    const Candidate *a = x, *b = y;
    int c = cost_cmp(a->cost, b->cost);
    return c ? c : (a->tie > b->tie) - (a->tie < b->tie);
}

static char *path_key(const int *path, int n) {
    Buf b;
    buf_init(&b);
    for (int i = 0; i < n; i++) buf_printf(&b, "%d,", path[i]);
    return buf_steal(&b);
}

/* For each route already found, every place on it is tried as a "spur": the part before it is
   kept, the edges that earlier routes used next are banned, and a fresh Dijkstra finds the rest.
   The best candidate becomes the next route. Returns how many routes were written to out[]. */
int k_shortest_paths(const Graph *g, int src, int dst, int mode, int k, PathResult **out) {
    PathResult *first = dijkstra(g, src, dst, mode, 0, NULL);
    if (!first) return 0;
    int found = 0;
    out[found++] = first;

    Heap candidates;
    heap_init(&candidates, sizeof(Candidate), candidate_cmp);
    StrMap *seen = sm_new(16);
    char *key = path_key(first->path, first->npath);
    sm_put(seen, key, NULL);
    free(key);
    int tie = 0;
    char *banned_nodes = xcalloc((size_t)g->n, 1);
    const Edge **banned = xmalloc((size_t)(k + 1) * sizeof *banned);

    while (found < k) {
        PathResult *last = out[found - 1];
        for (int i = 0; i < last->npath - 1; i++) {
            int spur = last->path[i];
            int nbanned = 0;
            for (int f = 0; f < found; f++) {
                PathResult *p = out[f];
                if (p->npath >= i + 1 && memcmp(p->path, last->path, (size_t)(i + 1) * sizeof(int)) == 0 &&
                    p->nedges > i)
                    banned[nbanned++] = p->edges[i];
            }
            memset(banned_nodes, 0, (size_t)g->n);
            for (int r = 0; r < i; r++) banned_nodes[last->path[r]] = 1;     /* keep the route loopless */

            PathResult *spur_route = search(g, spur, dst, mode, 0, NULL, NULL, NULL, banned, nbanned, banned_nodes);
            if (!spur_route) continue;

            PathResult *total = xcalloc(1, sizeof *total);
            total->npath = i + spur_route->npath;
            total->path = xmalloc((size_t)total->npath * sizeof(int));
            memcpy(total->path, last->path, (size_t)i * sizeof(int));
            memcpy(total->path + i, spur_route->path, (size_t)spur_route->npath * sizeof(int));
            total->nedges = i + spur_route->nedges;
            total->edges = xmalloc((size_t)(total->nedges ? total->nedges : 1) * sizeof(Edge *));
            memcpy(total->edges, last->edges, (size_t)i * sizeof(Edge *));
            memcpy(total->edges + i, spur_route->edges, (size_t)spur_route->nedges * sizeof(Edge *));
            path_free(spur_route);

            key = path_key(total->path, total->npath);
            if (sm_has(seen, key)) {
                free(key);
                path_free(total);
                continue;
            }
            sm_put(seen, key, NULL);
            free(key);
            double km = 0, min = 0;
            for (int e = 0; e < total->nedges; e++) {
                km += total->edges[e]->km;
                min += total->edges[e]->min;
            }
            total->km = py_round(km, 3);
            total->min = py_round(min, 3);
            heap_push(&candidates, &(Candidate){{total->km, total->min}, ++tie, total});
        }
        Candidate best;
        if (!heap_pop(&candidates, &best)) break;
        out[found++] = best.route;
    }
    Candidate rest;
    while (heap_pop(&candidates, &rest)) path_free(rest.route);
    heap_free(&candidates);
    sm_free(seen, NULL);
    free(banned_nodes);
    free(banned);
    return found;
}

/* ===================================================== journeys and ranking */

void journey_id(const Journey *j, char *out, size_t n) {
    if (j->variant == VAR_MIXED) snprintf(out, n, "mixed");
    else if (j->variant == VAR_FASTEST) snprintf(out, n, "%s_fastest", MODE_NAMES[j->mode]);
    else snprintf(out, n, "%s", MODE_NAMES[j->mode]);
}

void journey_clear(Journey *j) {
    free(j->path);
    free(j->edges);
    for (int i = 0; i < j->nlegs; i++) {
        free(j->legs[i].path);
        free(j->legs[i].edges);
    }
    free(j->legs);
    memset(j, 0, sizeof *j);
}

/* Sort key for a priority. Ties fall back to the other measure, then a fixed mode order. */
static void rank_key(const Journey *o, int priority, double key[3]) {
    double order = o->mode >= 0 ? o->mode : NMODES;          /* mixed journeys last */
    if (priority == PRI_FASTEST) {
        key[0] = o->min, key[1] = o->fare, key[2] = order;
    } else if (priority == PRI_BALANCED) {
        key[0] = py_round(o->fare + MINUTE_VALUE[PRI_BALANCED] * o->min, 6), key[1] = o->fare, key[2] = order;
    } else {
        key[0] = o->fare, key[1] = o->min, key[2] = order;
    }
}

int journey_rank_cmp(const Journey *x, const Journey *y, int priority) {
    double a[3], b[3];
    rank_key(x, priority, a);
    rank_key(y, priority, b);
    for (int i = 0; i < 3; i++) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

static void make_option(Journey *j, const PathResult *route, int mode, int variant) {
    memset(j, 0, sizeof *j);
    j->mode = mode;
    j->variant = variant;
    snprintf(j->label, sizeof j->label, "%s%s", mode_label(mode), variant == VAR_FASTEST ? " (quicker route)" : "");
    j->npath = route->npath;
    j->path = xmalloc((size_t)route->npath * sizeof(int));
    memcpy(j->path, route->path, (size_t)route->npath * sizeof(int));
    j->nedges = route->nedges;
    j->edges = xmalloc((size_t)(route->nedges ? route->nedges : 1) * sizeof(Edge *));
    memcpy(j->edges, route->edges, (size_t)route->nedges * sizeof(Edge *));
    j->km = route->km;
    j->min = route->min;
    j->fare = calculate_fare(mode, route->km);
}

/* ============================================ mixed-mode journeys ("Smart mix") */

/* State = (place, mode currently ridden); slot NMODES means "not riding anything yet". */
#define NSLOTS (NMODES + 1)

typedef struct {
    Cost cost;
    int node;
    int mode;                    /* -1 = none */
    const char *name;
} MItem;

static const char *mode_text(int mode) { return mode < 0 ? "" : MODE_NAMES[mode]; }

static int mitem_cmp(const void *x, const void *y) {
    const MItem *a = x, *b = y;
    int c = cost_cmp(a->cost, b->cost);
    if (c) return c;
    c = strcmp(a->name, b->name);
    return c ? c : strcmp(mode_text(a->mode), mode_text(b->mode));
}

/* Moves:
     ride an edge of the current mode: + per_km x km, + time
     board a different mode at the same place: + that mode's base fare, + transfer_min minutes
       (no transfer time for the very first boarding)
   Cost minimised = fare + minute_value x minutes. All costs are non-negative, so Dijkstra
   is still correct on this bigger state graph. */
Journey *mixed_route(const Graph *g, int src, int dst, double minute_value, double transfer_min) {
    if (src == dst) return NULL;
    int nstates = g->n * NSLOTS;
    Cost *best = xmalloc((size_t)nstates * sizeof *best);
    int *parent = xmalloc((size_t)nstates * sizeof *parent);
    const Edge **parent_edge = xcalloc((size_t)nstates, sizeof *parent_edge);
    char *done = xcalloc((size_t)nstates, 1);
    for (int i = 0; i < nstates; i++) {
        best[i] = COST_INF;
        parent[i] = -1;
    }
#define STATE(node, mode) ((node) * NSLOTS + ((mode) < 0 ? NMODES : (mode)))
    int start = STATE(src, -1);
    best[start] = (Cost){0, 0};

    Heap heap;
    heap_init(&heap, sizeof(MItem), mitem_cmp);
    heap_push(&heap, &(MItem){{0, 0}, src, -1, g->names[src]});
    int goal = -1;
    MItem it;
    while (heap_pop(&heap, &it)) {
        int state = STATE(it.node, it.mode);
        if (done[state] || cost_cmp(it.cost, best[state]) > 0) continue;
        done[state] = 1;
        if (it.node == dst && it.mode >= 0) {
            goal = state;
            break;
        }
        /* Board (or change to) another mode here. */
        for (int m = 0; m < NMODES; m++) {
            if (m == it.mode) continue;
            double extra_min = it.mode >= 0 ? transfer_min : 0.0;
            double step = FARE_RULES[m].base + minute_value * extra_min;
            int next = STATE(it.node, m);
            Cost cost = {it.cost.a + step, it.cost.b + extra_min};
            if (cost_cmp(cost, best[next]) < 0) {
                best[next] = cost;
                parent[next] = state;
                parent_edge[next] = NULL;
                heap_push(&heap, &(MItem){cost, it.node, m, g->names[it.node]});
            }
        }
        /* Ride the current mode. */
        if (it.mode >= 0) {
            for (int i = 0; i < g->deg[it.node]; i++) {
                const Edge *e = &g->adj[it.node][i];
                if (e->mode != it.mode) continue;
                double step = FARE_RULES[e->mode].per_km * e->km + minute_value * e->min;
                int next = STATE(e->to, e->mode);
                Cost cost = {it.cost.a + step, it.cost.b + e->min};
                if (cost_cmp(cost, best[next]) < 0) {
                    best[next] = cost;
                    parent[next] = state;
                    parent_edge[next] = e;
                    heap_push(&heap, &(MItem){cost, e->to, e->mode, g->names[e->to]});
                }
            }
        }
    }
    heap_free(&heap);

    Journey *j = NULL;
    if (goal >= 0) {
        /* Rebuild: walk back through the states, then group rides of the same mode into legs. */
        int nsteps = 0;
        for (int s = goal; s != start; s = parent[s]) nsteps++;
        int *steps = xmalloc((size_t)(nsteps ? nsteps : 1) * sizeof(int));
        int k = nsteps;
        for (int s = goal; s != start; s = parent[s]) steps[--k] = s;

        MixLeg *legs = xcalloc((size_t)(nsteps ? nsteps : 1), sizeof *legs);
        int nlegs = 0;
        j = xcalloc(1, sizeof *j);
        j->path = xmalloc((size_t)(nsteps + 1) * sizeof(int));
        j->edges = xmalloc((size_t)(nsteps ? nsteps : 1) * sizeof(Edge *));
        j->path[j->npath++] = src;
        for (int i = 0; i < nsteps; i++) {
            int s = steps[i];
            const Edge *e = parent_edge[s];
            if (!e) {                                   /* boarding a new mode */
                MixLeg *leg = &legs[nlegs++];
                leg->mode = s % NSLOTS;
                leg->path = xmalloc((size_t)(nsteps + 1) * sizeof(int));
                leg->edges = xmalloc((size_t)(nsteps ? nsteps : 1) * sizeof(Edge *));
                leg->path[leg->npath++] = s / NSLOTS;
                continue;
            }
            MixLeg *leg = &legs[nlegs - 1];
            leg->path[leg->npath++] = e->to;
            leg->edges[leg->nedges++] = e;
            j->path[j->npath++] = e->to;
            j->edges[j->nedges++] = e;
        }
        free(steps);
        /* Drop a boarding that was never used, then total up each leg. */
        int kept = 0;
        for (int i = 0; i < nlegs; i++) {
            if (legs[i].nedges == 0) {
                free(legs[i].path);
                free(legs[i].edges);
                continue;
            }
            legs[kept++] = legs[i];
        }
        nlegs = kept;
        double total_km = 0, total_min = 0, total_fare = 0;
        Buf label;
        buf_init(&label);
        buf_puts(&label, "Smart mix: ");
        for (int i = 0; i < nlegs; i++) {
            MixLeg *leg = &legs[i];
            double km = 0, min = 0;
            for (int e = 0; e < leg->nedges; e++) {
                km += leg->edges[e]->km;
                min += leg->edges[e]->min;
            }
            leg->km = py_round(km, 3);
            leg->min = py_round(min, 3);
            leg->fare = calculate_fare(leg->mode, leg->km);
            total_km += leg->km;
            total_min += leg->min;
            total_fare += leg->fare;
            if (i) buf_puts(&label, " → ");
            buf_puts(&label, mode_label(leg->mode));
        }
        int transfers = nlegs > 1 ? nlegs - 1 : 0;
        j->mode = -1;
        j->variant = VAR_MIXED;
        snprintf(j->label, sizeof j->label, "%s", label.s);
        buf_free(&label);
        j->km = py_round(total_km, 3);
        j->min = py_round(total_min + transfers * transfer_min, 3);
        j->fare = py_round(total_fare, 2);
        j->legs = legs;
        j->nlegs = nlegs;
    }
#undef STATE
    free(best);
    free(parent);
    free(parent_edge);
    free(done);
    return j;
}

/* ============================================================ comparison */

static void add_option(Comparison *c, Journey *j) {
    c->opts = xrealloc(c->opts, (size_t)(c->nopts + 1) * sizeof *c->opts);
    c->opts[c->nopts++] = *j;
}

/* For each mode: its cheapest route (shortest distance) and, if a different route is quicker,
   that quicker route too. Plus one "Smart mix" when changing modes beats every single-mode trip
   for the chosen priority, or when no single mode can make the trip at all. */
void compare_routes(const Graph *g, int src, int dst, int priority, int include_extras, Comparison *out) {
    memset(out, 0, sizeof *out);
    out->src = src;
    out->dst = dst;
    out->priority = priority;
    if (src == dst) return;

    for (int mode = 0; mode < NMODES; mode++) {
        PathResult *cheapest = dijkstra(g, src, dst, mode, 0, NULL);
        if (!cheapest) {
            out->unavailable[out->nunavailable++] = mode;
            continue;
        }
        Journey j;
        make_option(&j, cheapest, mode, VAR_CHEAPEST);
        add_option(out, &j);
        if (include_extras) {
            PathResult *quickest = dijkstra(g, src, dst, mode, 1, NULL);
            if (quickest && quickest->min < cheapest->min - 1e-9) {
                make_option(&j, quickest, mode, VAR_FASTEST);
                add_option(out, &j);
            }
            path_free(quickest);
        }
        path_free(cheapest);
    }

    if (include_extras) {
        Journey *mix = mixed_route(g, src, dst, MINUTE_VALUE[priority], TRANSFER_MIN);
        int show = 0;
        if (mix && mix->nlegs > 1) {
            show = 1;
            for (int i = 0; i < out->nopts && show; i++)
                if (journey_rank_cmp(mix, &out->opts[i], priority) >= 0) show = 0;
        }
        if (show) {
            add_option(out, mix);
            free(mix);
        } else if (mix) {
            journey_clear(mix);
            free(mix);
        }
    }

    /* Stable insertion sort by the priority's key (equal keys keep their order). */
    for (int i = 1; i < out->nopts; i++) {
        Journey key = out->opts[i];
        int k = i - 1;
        while (k >= 0 && journey_rank_cmp(&out->opts[k], &key, priority) > 0) {
            out->opts[k + 1] = out->opts[k];
            k--;
        }
        out->opts[k + 1] = key;
    }
}

void comparison_free(Comparison *c) {
    for (int i = 0; i < c->nopts; i++) journey_clear(&c->opts[i]);
    free(c->opts);
    c->opts = NULL;
    c->nopts = 0;
}

static int pick(const Comparison *c, int priority) {
    int best = -1;
    for (int i = 0; i < c->nopts; i++)
        if (best < 0 || journey_rank_cmp(&c->opts[i], &c->opts[best], priority) < 0) best = i;
    return best;
}

/* Exactly one option each: lowest fare (then quicker, then mode order) / quickest (then cheaper). */
int comparison_cheapest(const Comparison *c) { return pick(c, PRI_CHEAPEST); }
int comparison_fastest(const Comparison *c) { return pick(c, PRI_FASTEST); }
