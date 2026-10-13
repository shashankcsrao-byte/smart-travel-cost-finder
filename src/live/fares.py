"""
Published fare rules for Bengaluru used in live mode.

Every rule records where it comes from and how reliable it is ("kind"):
    official    - the operator's own fare table (Namma Metro, from GTFS data)
    regulated   - a Government of Karnataka notified rate
    estimated   - derived from older official data using a verified ratio (shown with "≈")
    indicative  - no official rate card exists; editable assumption

Live app prices for Uber, Ola, Rapido and Namma Yatri are NOT available to
third-party apps, so road fares here are reference estimates. The app links to
each service so the user can see the exact current price.
"""

import math

AUTO_METER = {
    "label": "Auto meter fare",
    "kind": "regulated",
    "min_fare": 36, "min_km": 2, "per_km": 18,
    "night_multiplier": 1.5, "night_start_hour": 22, "night_end_hour": 5,
    "source": "Govt. of Karnataka auto-rickshaw fare revision, effective 1 Aug 2025 "
              "(₹36 for first 2 km, ₹18/km after; +50% from 10 PM to 5 AM)",
}

CAB = {
    "label": "Cab (vehicle under ₹10 lakh)",
    "kind": "regulated",
    "min_fare": 100, "min_km": 4, "per_km": 24,
    "source": "Govt. of Karnataka uniform taxi fare notification, Feb 2025 "
              "(₹100 for first 4 km, ₹24/km after). App prices can differ.",
}

BIKE_TAXI = {
    "label": "Bike taxi",
    "kind": "indicative",
    "min_fare": 20, "min_km": 0, "per_km": 8,
    "source": "No official bike-taxi rate card is published for Karnataka. "
              "Indicative values only - edit src/live/fares.py.",
}

BMTC_ORDINARY = {
    "label": "BMTC ordinary bus",
    # (up to km, fare). First three stages: Jan 2025 revision as publicly reported.
    "verified_stages": [(2, 6), (4, 12), (6, 18)],
    # Longer trips: the 2020 BMTC slabs (₹20 for 7-14 km, ₹25 for 15-40 km, ₹30 above 40 km)
    # x 1.2, because the verified 2025 stages are exactly 1.2 x the 2020 ones (5->6, 10->12, 15->18).
    "estimated_stages": [(14, 24), (40, 30), (float("inf"), 36)],
    "source": "BMTC ordinary fares: ₹6 / ₹12 / ₹18 up to 6 km (Jan 2025 revision). Longer trips are "
              "estimated as the 2020 slabs x 1.2 (≈₹24 up to 14 km, ≈₹30 up to 40 km, ≈₹36 beyond). "
              "Check the official BMTC fare chart for the exact amount.",
}

# Assumptions used to estimate bus travel time from live road time.
BUS_SLOWDOWN = 1.5          # buses stop often and are slower than cars
BUS_AVG_WAIT_MIN = 8.0
WALK_KMPH = 4.5
MAX_WALK_KM = 1.0           # longer first/last-mile legs use an auto


def is_night(hour, rule=AUTO_METER):
    return hour >= rule["night_start_hour"] or hour < rule["night_end_hour"]


def distance_fare(rule, distance_km):
    """Minimum fare covers the first `min_km`; each further km costs `per_km`."""
    extra_km = max(0.0, distance_km - rule["min_km"])
    return rule["min_fare"] + rule["per_km"] * extra_km


def auto_fare(distance_km, hour):
    fare = distance_fare(AUTO_METER, distance_km)
    night = is_night(hour)
    if night:
        fare *= AUTO_METER["night_multiplier"]
    return round(fare), night


def cab_fare(distance_km):
    return round(distance_fare(CAB, distance_km))


def bike_fare(distance_km):
    return round(distance_fare(BIKE_TAXI, distance_km))


def bmtc_fare(distance_km):
    """(fare, exact). exact=False means the fare is an estimate (trips over 6 km)."""
    for up_to_km, fare in BMTC_ORDINARY["verified_stages"]:
        if distance_km <= up_to_km:
            return fare, True
    for up_to_km, fare in BMTC_ORDINARY["estimated_stages"]:
        if distance_km <= up_to_km:
            return fare, False
    return BMTC_ORDINARY["estimated_stages"][-1][1], False


def walk_minutes(distance_km):
    return math.ceil(distance_km / WALK_KMPH * 60)
