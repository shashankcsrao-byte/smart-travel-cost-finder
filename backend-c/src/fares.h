/*
 * fares.h - fare rules.
 *
 * Demo city: illustrative sample fares, fare = base + per_km x distance.
 * Live Bengaluru: published rules with their sources (Govt. of Karnataka auto and
 * cab rates, BMTC stages, an indicative bike-taxi rate). Namma Metro fares come
 * from the official table inside the GTFS feed (see metro.c).
 */
#ifndef FARES_H
#define FARES_H

#include "graph.h"

/* ---- demo city ---- */
typedef struct {
    const char *label;
    double base, per_km;
} FareRule;

extern const FareRule FARE_RULES[NMODES];
double calculate_fare(int mode, double km);       /* round(base + per_km * km, 2) */
const char *mode_label(int mode);

/* ---- live Bengaluru ---- */
#define AUTO_MIN_FARE 36
#define AUTO_MIN_KM 2
#define AUTO_PER_KM 18
#define AUTO_NIGHT_MULTIPLIER 1.5
#define CAB_MIN_FARE 100
#define CAB_MIN_KM 4
#define CAB_PER_KM 24
#define BIKE_MIN_FARE 20
#define BIKE_PER_KM 8

#define BUS_SLOWDOWN 1.5          /* buses stop often and are slower than cars */
#define BUS_AVG_WAIT_MIN 8.0
#define WALK_KMPH 4.5
#define MAX_WALK_KM 1.0           /* longer first/last-mile legs use an auto */
#define WALK_OPTION_MAX_KM 3.0    /* offer "just walk" for trips up to this road distance */
#define MAX_TRAIN_WAIT_MIN 45     /* waiting longer than this for the first train = "no train now" */

extern const char *const AUTO_SOURCE, *const CAB_SOURCE, *const BIKE_SOURCE, *const BMTC_SOURCE;

int is_night(int hour);
long auto_fare(double km, int hour, int *night);
long cab_fare(double km);
long bike_fare(double km);
long bmtc_fare(double km, int *exact);           /* exact = 0 means estimated (over 6 km) */
long walk_minutes(double km);

#endif
