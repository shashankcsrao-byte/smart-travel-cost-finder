/*
 * planner.h - live trip planner for Bengaluru.
 *
 * For one origin, destination and departure time it builds an option per mode:
 *     Walk       short trips only, free
 *     Auto       live road route (Ola Maps) + Govt auto meter fare
 *     Cab        live road route (Ola Maps) + Govt-notified cab fare
 *     Bike taxi  live road route (Ola Maps) + indicative fare
 *     Bus        live road time x slowdown + BMTC stage fare (estimate)
 *     Metro      walk/auto to a station + timetable Dijkstra on the REAL train times
 *                + official BMRCL fare + walk/auto from the exit station
 * Options are ranked by the priority; exactly one is marked cheapest and one fastest.
 */
#ifndef PLANNER_H
#define PLANNER_H

#include <time.h>

#include "metro.h"
#include "olamaps.h"
#include "util.h"

typedef struct {
    char *name;
    double lat, lng;
} Place;

/* Writes the whole JSON answer into out. Returns 0, or -1 with err filled in. */
int plan_trip(OlaClient *maps, const MetroNetwork *metro, const Place *origin, const Place *destination,
              time_t depart, int priority, Buf *out, char cheapest_mode[16], double *cheapest_fare, OlaError *err);

#endif
