/*
 * fares.c - see fares.h.
 */
#include "fares.h"

#include <math.h>

#include "util.h"

/* ILLUSTRATIVE SAMPLE FARES ONLY - not official BMTC, Namma Metro, Rapido, Uber or Ola fares. */
const FareRule FARE_RULES[NMODES] = {
    [MODE_BUS] = {"Bus", 5, 2},
    [MODE_METRO] = {"Metro", 10, 3},
    [MODE_BIKE_TAXI] = {"Bike Taxi", 20, 8},
    [MODE_CAR_TAXI] = {"Car Taxi", 40, 14},
};

/* The base fare is charged ONCE per journey, never once per edge. */
double calculate_fare(int mode, double km) {
    return py_round(FARE_RULES[mode].base + FARE_RULES[mode].per_km * km, 2);
}

const char *mode_label(int mode) { return FARE_RULES[mode].label; }

/* ------------------------------------------------------------------- live */

const char *const AUTO_SOURCE =
    "Govt. of Karnataka auto-rickshaw fare revision, effective 1 Aug 2025 "
    "(₹36 for first 2 km, ₹18/km after; +50% from 10 PM to 5 AM)";
const char *const CAB_SOURCE =
    "Govt. of Karnataka uniform taxi fare notification, Feb 2025 "
    "(₹100 for first 4 km, ₹24/km after). App prices can differ.";
const char *const BIKE_SOURCE =
    "No official bike-taxi rate card is published for Karnataka, so this is an "
    "indicative estimate. Check the app for the real price.";
const char *const BMTC_SOURCE =
    "BMTC ordinary fares: ₹6 / ₹12 / ₹18 up to 6 km (Jan 2025 revision). Longer trips are "
    "estimated as the 2020 slabs x 1.2 (≈₹24 up to 14 km, ≈₹30 up to 40 km, ≈₹36 beyond). "
    "Check the official BMTC fare chart for the exact amount.";

/* Minimum fare covers the first min_km; each further km costs per_km. */
static double distance_fare(double min_fare, double min_km, double per_km, double km) {
    double extra = km - min_km;
    return min_fare + per_km * (extra > 0 ? extra : 0);
}

int is_night(int hour) { return hour >= 22 || hour < 5; }

long auto_fare(double km, int hour, int *night) {
    double fare = distance_fare(AUTO_MIN_FARE, AUTO_MIN_KM, AUTO_PER_KM, km);
    int n = is_night(hour);
    if (n) fare *= AUTO_NIGHT_MULTIPLIER;
    if (night) *night = n;
    return py_round_int(fare);
}

long cab_fare(double km) { return py_round_int(distance_fare(CAB_MIN_FARE, CAB_MIN_KM, CAB_PER_KM, km)); }

long bike_fare(double km) { return py_round_int(distance_fare(BIKE_MIN_FARE, 0, BIKE_PER_KM, km)); }

long bmtc_fare(double km, int *exact) {
    /* First three stages: Jan 2025 revision. Longer trips: the 2020 slabs x 1.2, because the
       verified 2025 stages are exactly 1.2 x the 2020 ones (5 -> 6, 10 -> 12, 15 -> 18). */
    static const double verified[][2] = {{2, 6}, {4, 12}, {6, 18}};
    static const double estimated[][2] = {{14, 24}, {40, 30}, {INFINITY, 36}};
    for (int i = 0; i < 3; i++)
        if (km <= verified[i][0]) {
            *exact = 1;
            return (long)verified[i][1];
        }
    *exact = 0;
    for (int i = 0; i < 3; i++)
        if (km <= estimated[i][0]) return (long)estimated[i][1];
    return 36;
}

long walk_minutes(double km) { return (long)ceil(km / WALK_KMPH * 60); }
