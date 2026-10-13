"""
Fare rules and fare calculation.

ILLUSTRATIVE SAMPLE FARES ONLY - these are not official BMTC, Namma Metro,
Rapido, Uber or Ola fares.

Single-mode journey fare:

    total fare = base fare + (per-km rate x total route distance)

The base fare is charged ONCE per journey, never once per edge.
"""

from .graph import MODES

FARE_RULES = {
    "bus": {"label": "Bus", "base": 5, "per_km": 2},
    "metro": {"label": "Metro", "base": 10, "per_km": 3},
    "bike_taxi": {"label": "Bike Taxi", "base": 20, "per_km": 8},
    "car_taxi": {"label": "Car Taxi", "base": 40, "per_km": 14},
}

# Sanity check at import time: every mode must have a fare rule.
if set(FARE_RULES) != set(MODES):
    raise RuntimeError("FARE_RULES must define exactly the modes listed in MODES")


def mode_label(mode):
    """Human-friendly name of a mode, e.g. 'bike_taxi' -> 'Bike Taxi'."""
    return FARE_RULES[mode]["label"]


def calculate_fare(mode, distance_km, fare_rules=FARE_RULES):
    """
    Fare for a whole single-mode journey of `distance_km` kilometres.

    >>> calculate_fare("bus", 10)   # 5 + 2 * 10
    25
    """
    if mode not in fare_rules:
        raise ValueError(f"Unknown transport mode: {mode!r}")
    if distance_km < 0:
        raise ValueError("Distance cannot be negative")

    rule = fare_rules[mode]
    return round(rule["base"] + rule["per_km"] * distance_km, 2)
