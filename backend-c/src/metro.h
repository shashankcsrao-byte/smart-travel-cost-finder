/*
 * metro.h - the real Namma Metro network built from GTFS data, routed with OUR algorithms.
 *
 * Data: Namma Metro GTFS by Vonter (github.com/Vonter/bmrcl-gtfs), ODbL licence,
 * built from BMRCL timetables and OpenStreetMap. Includes official BMRCL fares.
 *
 * Two ways to route, both written by hand:
 *
 * 1. Timetable search (metro_journey, metro_write_trace): a time-dependent Dijkstra.
 *    A state is "standing at station S on line L at time t". From there you can
 *      - take the NEXT train of line L in either direction (found by binary search
 *        over that station's sorted departure times) to the following station, or
 *      - walk to another line's platform at the same station (+1 minute).
 *    Arriving earlier never makes you arrive later ("FIFO"), so Dijkstra stays correct.
 *    It uses the real calendar (weekday / Monday / Sunday / holiday timetables), and
 *    trains after midnight belong to the previous day's service.
 *
 * 2. Static average-wait graph (metro_route): used when no trains run (late night):
 *        IN:A -> A@LINE (half the headway), A@LINE -> B@LINE (ride),
 *        A@LINE1 -> A@LINE2 (change lines), A@LINE -> OUT:A
 */
#ifndef METRO_H
#define METRO_H

#include <stddef.h>
#include <time.h>

#include "graph.h"
#include "json.h"
#include "strmap.h"

#define METRO_DAY 86400
#define INTERCHANGE_WALK_MIN 1.0     /* GTFS transfers.txt: min_transfer_time = 60 s */
#define DEFAULT_WAIT_MIN 5.0         /* used if a line's headway cannot be derived */

typedef struct {
    char *id, *name;
    double lat, lng;
    int lines[8];                    /* line numbers serving this station */
    int nlines;
} Station;

typedef struct {
    char *id, *name, *color;
    int *stations;                   /* in direction 0 */
    int nstations;
    double wait_min;                 /* half the weekday daytime headway */
} Line;

typedef struct {
    int station;
    int arr, dep;                    /* seconds after midnight (may exceed 24 h) */
    double dist;                     /* shape_dist_traveled, km */
} Call;

typedef struct {
    char *id;
    int line, dir, service;
    Call *calls;
    int ncalls;
} Trip;

typedef struct {
    int dep, trip, idx;
} Dep;

typedef struct {
    Dep *d;
    int n, cap;
} DepList;

typedef struct {
    int service;
    char days[7];                    /* Monday .. Sunday: 1 if it runs */
    char start[16], end[16];         /* YYYYMMDD */
} CalRow;

typedef struct {
    char *group;
    char start[16], end[16];         /* HH:MM:SS */
    int service;
} Timeframe;

typedef struct {
    Station *stations;
    int nstations;
    StrMap *station_index;
    Line *lines;
    int nlines;
    StrMap *line_index;
    Trip *trips;
    int ntrips;
    char **services;
    int nservices;
    StrMap *service_index;
    DepList *deps;                   /* per (service, line, direction, station), sorted */
    CalRow *calendar;
    int ncalendar;
    StrMap *exceptions;              /* "YYYYMMDD" -> list of (service, type) */
    Timeframe *timeframes;
    int ntimeframes;
    StrMap *token_fare;              /* "A|B" -> price */
    StrMap *card_fare;               /* "A|B|peak" -> price */
    StrMap *hop_km;                  /* "A|B|LINE" -> km */
    Graph *graph;                    /* the static average-wait graph */
    int *node_station, *node_line;   /* graph vertex -> station / line (-1 for IN: and OUT:) */
    double top_speed_km_s;           /* fastest hop: makes the A* estimate admissible */
    int has_feed_end;
    long feed_end_days;
} MetroNetwork;

typedef struct {
    int line;
    int *stations;
    int nstations, cap;
    double km, min;
    int timed;                       /* real clock times known */
    double board_s, alight_s;
} Ride;

typedef struct {
    int origin, destination;
    Ride *rides;
    int nrides;
    double km, min;
    int timetable;
    double first_wait_min;
    double depart_s, arrive_s;
    int pops;
} MetroJourney;

typedef struct {
    int station;
    double km;
} Near;

typedef struct {
    int has_token, has_card;
    double token, card;
    const char *timeframe;
} MetroFare;

MetroNetwork *metro_load(const char *zip_path, char *err, size_t errlen);
void metro_free(MetroNetwork *m);

int metro_station(const MetroNetwork *m, const char *id);        /* -1 if unknown */
int metro_nearest(const MetroNetwork *m, double lat, double lng, int k, double max_km, Near *out);
void metro_fare(const MetroNetwork *m, int o, int d, time_t when, MetroFare *out);
long metro_feed_days_left(const MetroNetwork *m, time_t now, int *has);

MetroJourney *metro_journey(const MetroNetwork *m, int o, int d, time_t depart, int use_astar);
MetroJourney *metro_route(const MetroNetwork *m, int o, int d);
void metro_journey_free(MetroJourney *j);
int metro_next_departure(const MetroNetwork *m, int station, time_t depart, char out[8]);   /* 0 if none */

void metro_clock(double seconds, char out[8]);                   /* "HH:MM" */
void metro_write_station(const MetroNetwork *m, JW *w, int s);
void metro_write_ride(const MetroNetwork *m, JW *w, const Ride *r);
void metro_write_journey(const MetroNetwork *m, JW *w, const MetroJourney *j);
void metro_write_fare(JW *w, const MetroFare *f);
void metro_write_network(const MetroNetwork *m, JW *w);          /* stations + lines for the map */
void metro_write_trace(const MetroNetwork *m, int o, int d, time_t when, int use_astar, JW *w);

#endif
