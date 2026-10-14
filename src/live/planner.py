"""
Live trip planner for Bengaluru.

For one origin, destination and departure time it builds an option per mode:

    Walk       short trips only, free
    Auto       live road route (Ola Maps) + Govt auto meter fare
    Cab        live road route (Ola Maps) + Govt-notified cab fare
    Bike taxi  live road route (Ola Maps) + indicative fare
    Bus        live road time x slowdown + BMTC stage fare (estimate)
    Metro      walk/auto to a station + timetable Dijkstra on the REAL train times
               + official BMRCL fare + walk/auto from the exit station

Options are ranked by the visitor's priority (cheapest, balanced or fastest).
Exactly one option is marked cheapest and exactly one fastest.
"""

import json
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta
from urllib.parse import urlencode

from ..routing import MINUTE_VALUE
from . import fares as F
from .metro import IST, haversine_km
from .olamaps import OlaMapsError

MODE_ORDER = ("walk", "bus", "metro", "auto", "bike_taxi", "cab")


# ---------------------------------------------------------------- deep links

def uber_link(origin, destination):
    """Uber universal link with pickup and drop pre-filled (developer.uber.com deep links)."""
    place = lambda p: json.dumps({"latitude": p["lat"], "longitude": p["lng"],
                                  "addressLine1": p["name"]}, separators=(",", ":"))
    return "https://m.uber.com/looking?" + urlencode({"pickup": place(origin), "drop[0]": place(destination)})


def maps_link(origin, destination, travelmode="transit"):
    """Google Maps URL (no API key needed) for directions."""
    return "https://www.google.com/maps/dir/?" + urlencode({
        "api": 1, "origin": f"{origin['lat']},{origin['lng']}",
        "destination": f"{destination['lat']},{destination['lng']}", "travelmode": travelmode})


maps_transit_link = maps_link          # backwards-compatible name

APP_LINKS = {
    "namma_yatri": {"label": "Namma Yatri", "url": "https://nammayatri.in/"},
    "ola": {"label": "Ola", "url": "https://www.olacabs.com/"},
    "rapido": {"label": "Rapido", "url": "https://www.rapido.bike/"},
}


# ------------------------------------------------------------------- ranking

def rank_key(option, priority="cheapest"):
    order = MODE_ORDER.index(option["mode"])
    if priority == "fastest":
        return (option["time_min"], option["fare"], order)
    if priority == "balanced":
        return (round(option["fare"] + MINUTE_VALUE["balanced"] * option["time_min"], 6), option["fare"], order)
    return (option["fare"], option["time_min"], order)


# ------------------------------------------------------------------- helpers

def _leg(kind, start, end, km, minutes, **extra):
    return {"kind": kind, "from": start, "to": end,
            "distance_km": round(km, 2), "time_min": round(minutes, 1), **extra}


def _access_leg(place_name, station, road, hour, towards_station):
    """Walk if short, otherwise take an auto. Returns (leg, fare)."""
    start, end = (place_name, station["name"]) if towards_station else (station["name"], place_name)
    km = road["distance_km"]
    if km <= F.MAX_WALK_KM:
        return _leg("walk", start, end, km, F.walk_minutes(km)), 0
    fare, _ = F.auto_fare(km, hour)
    return _leg("auto", start, end, km, road["duration_min"], fare=fare), fare


def _approx_road(a, b):
    """Fallback if the distance matrix call fails: straight line x 1.3 at 20 km/h."""
    km = haversine_km(a, b) * 1.3
    return {"distance_km": round(km, 2), "duration_min": round(km / 20 * 60, 1)}


# -------------------------------------------------------------------- metro

def plan_metro(maps, metro, origin, destination, depart, priority="cheapest"):
    """Best metro journey for the priority, using real train times. (option, reason)."""
    o_point, d_point = (origin["lat"], origin["lng"]), (destination["lat"], destination["lng"])
    starts = metro.nearest_stations(o_point, k=3)
    ends = metro.nearest_stations(d_point, k=3)
    if not starts or not ends:
        return None, "No metro station within 6 km of the start or the destination"
    if {s["id"] for s, _ in starts} == {s["id"] for s, _ in ends}:
        return None, "Both places are next to the same stations, so the metro does not help"

    station_point = lambda s: (s["lat"], s["lng"])
    approximate = False
    try:
        # Both lookups at once: each is a network round trip to Ola Maps.
        with ThreadPoolExecutor(max_workers=2) as pool:
            to_job = pool.submit(maps.distance_matrix, [o_point], [station_point(s) for s, _ in starts])
            from_job = pool.submit(maps.distance_matrix, [station_point(s) for s, _ in ends], [d_point])
            to_station = to_job.result()[0]
            from_station = [row[0] for row in from_job.result()]
    except OlaMapsError:
        approximate = True
        to_station, from_station = [None] * len(starts), [None] * len(ends)

    hour = depart.astimezone(IST).hour
    minute_value = MINUTE_VALUE[priority]
    best, waits = None, []
    for (a, _), road_a in zip(starts, to_station):
        road_a = road_a or _approx_road(o_point, station_point(a))
        first_leg, first_fare = _access_leg(origin["name"], a, road_a, hour, True)
        at_station = depart + timedelta(minutes=first_leg["time_min"])
        for (b, _), road_b in zip(ends, from_station):
            if a["id"] == b["id"]:
                continue
            road_b = road_b or _approx_road(station_point(b), d_point)
            journey = metro.journey(a["id"], b["id"], at_station)
            if journey is None or journey["first_wait_min"] > F.MAX_TRAIN_WAIT_MIN:
                waits.append((a, at_station))
                continue
            fare_info = metro.fare(a["id"], b["id"], at_station)
            if fare_info["token"] is None:
                continue
            last_leg, last_fare = _access_leg(destination["name"], b, road_b, hour, False)
            total_fare = fare_info["token"] + first_fare + last_fare
            total_time = first_leg["time_min"] + journey["time_min"] + last_leg["time_min"]
            score = (total_fare + minute_value * total_time, total_time)
            if best is None or score < best[0]:
                best = (score, total_fare, total_time, first_leg, journey, fare_info, last_leg)

    if best is None:
        if waits:
            station, at_station = waits[0]
            when = metro.next_departure(station["id"], at_station)
            soon = when and (datetime.strptime(when, "%H:%M") - datetime.strptime(at_station.strftime("%H:%M"), "%H:%M")
                             ).seconds / 60 <= F.MAX_TRAIN_WAIT_MIN
            if soon:
                # A train is coming, but the connections needed to finish the trip have stopped.
                return None, "Too late to finish this trip by metro tonight (the last connection has gone)"
            return None, (f"No metro train soon from {station['name']}"
                          + (f". First train at {when}" if when else ". No more trains today"))
        return None, "No useful metro route between these places"

    _score, total_fare, total_time, first_leg, journey, fare_info, last_leg = best
    legs = [first_leg]
    for ride in journey["rides"]:
        legs.append(_leg("metro", ride["from"], ride["to"], ride["distance_km"], ride["time_min"],
                         line=ride["line_name"], color=ride["color"], stops=ride["stops"], coords=ride["coords"],
                         from_id=ride["stations"][0], to_id=ride["stations"][-1],
                         depart=ride["depart"], arrive=ride["arrive"]))
    legs.append(last_leg)

    rides = journey["rides"]
    notes = [f"Real timetable: board at {rides[0]['depart']}"
             + (f" (about {round(journey['first_wait_min'])} min wait)" if journey["first_wait_min"] >= 1 else "")
             + (f", {journey['changes']} line change(s)" if journey["changes"] else "")
             + f", arrive {rides[-1]['arrive']}."]
    if fare_info["smart_card"]:
        notes.append(f"Smart card: ₹{fare_info['smart_card']:g} ({fare_info['timeframe']}) instead of ₹{fare_info['token']:g}.")
    if first_leg["kind"] == "auto" or last_leg["kind"] == "auto":
        notes.append("Includes auto fare (meter rate) to/from the station.")
    if approximate:
        notes.append("Station access distances are approximate (map service unavailable).")

    auto_fares = first_leg.get("fare", 0) + last_leg.get("fare", 0)
    return {
        "mode": "metro", "label": "Namma Metro", "providers": "BMRCL",
        "fare": round(total_fare), "fare_kind": "official",
        "fare_text": f"₹{fare_info['token']:g} metro" + (f" + ₹{auto_fares} auto" if auto_fares else ""),
        "fare_source": "Official BMRCL station-to-station fare (Namma Metro GTFS)",
        "distance_km": round(sum(leg["distance_km"] for leg in legs), 2), "time_min": round(total_time, 1),
        "legs": legs, "notes": notes,
        "links": [{"label": "Directions in Google Maps", "url": maps_link(origin, destination)}],
    }, None


# --------------------------------------------------------------------- plan

def plan_trip(maps, metro, origin, destination, now=None, priority="cheapest"):
    depart = (now or datetime.now(IST)).astimezone(IST)
    hour = depart.hour
    # The metro search needs no road data, so it runs while the road route is fetched.
    with ThreadPoolExecutor(max_workers=1) as pool:
        metro_job = pool.submit(plan_metro, maps, metro, origin, destination, depart, priority) if metro else None
        road = maps.directions((origin["lat"], origin["lng"]), (destination["lat"], destination["lng"]))
        metro_option, metro_reason = metro_job.result() if metro_job else (None, None)
    km, minutes = road["distance_km"], road["duration_min"]
    road_leg = _leg("road", origin["name"], destination["name"], km, minutes, polyline=road["polyline"])

    options, unavailable = [], []

    if km <= F.WALK_OPTION_MAX_KM:
        options.append({
            "mode": "walk", "label": "Walk", "providers": "On foot",
            "fare": 0, "fare_kind": "free", "fare_text": "Free",
            "fare_source": f"Walking at {F.WALK_KMPH:g} km/h along the road route.",
            "distance_km": km, "time_min": F.walk_minutes(km),
            "legs": [{**road_leg, "kind": "walk", "time_min": F.walk_minutes(km)}],
            "notes": ["Short trip: walking is free and often as quick as waiting for a ride."],
            "links": [{"label": "Walking directions", "url": maps_link(origin, destination, "walking")}],
        })

    auto, night = F.auto_fare(km, hour)
    options.append({
        "mode": "auto", "label": "Auto", "providers": "Namma Yatri · meter autos",
        "fare": auto, "fare_kind": "regulated",
        "fare_text": "Meter fare" + (", night +50%" if night else ""),
        "fare_source": F.AUTO_METER["source"], "distance_km": km, "time_min": minutes,
        "legs": [{**road_leg, "kind": "auto"}],
        "notes": ["Live travel time from Ola Maps (includes current traffic)."],
        "links": [APP_LINKS["namma_yatri"], {"label": "Uber", "url": uber_link(origin, destination)}],
    })
    options.append({
        "mode": "cab", "label": "Cab", "providers": "Uber · Ola",
        "fare": F.cab_fare(km), "fare_kind": "regulated", "fare_text": "App price may differ (surge, fees)",
        "fare_source": F.CAB["source"], "distance_km": km, "time_min": minutes,
        "legs": [{**road_leg, "kind": "cab"}],
        "notes": ["App prices may include surge and fees. Tap a button for the exact fare."],
        "links": [{"label": "Uber", "url": uber_link(origin, destination)}, APP_LINKS["ola"]],
    })
    options.append({
        "mode": "bike_taxi", "label": "Bike taxi", "providers": "Rapido · Uber Moto",
        "fare": F.bike_fare(km), "fare_kind": "indicative", "fare_text": "Check the app for the actual price",
        "fare_source": F.BIKE_TAXI["source"], "distance_km": km, "time_min": minutes,
        "legs": [{**road_leg, "kind": "bike_taxi"}],
        "notes": ["No official rate card exists. Check the app for the real price."],
        "links": [APP_LINKS["rapido"], {"label": "Uber", "url": uber_link(origin, destination)}],
    })

    bus_fare, exact = F.bmtc_fare(km)
    bus_time = round(minutes * F.BUS_SLOWDOWN + F.BUS_AVG_WAIT_MIN, 1)
    options.append({
        "mode": "bus", "label": "BMTC bus", "providers": "BMTC ordinary",
        "fare": bus_fare, "fare_kind": "official" if exact else "estimated",
        "fare_text": "Stage fare" if exact else "Estimated stage fare; check the BMTC chart",
        "fare_source": F.BMTC_ORDINARY["source"], "distance_km": km, "time_min": bus_time,
        "legs": [{**road_leg, "kind": "bus", "time_min": bus_time}],
        "notes": [f"Estimated: road time × {F.BUS_SLOWDOWN} + {F.BUS_AVG_WAIT_MIN:g} min average wait. "
                  "Actual routes may need a change of bus."],
        "links": [{"label": "Bus routes in Google Maps", "url": maps_link(origin, destination)}],
    })

    if metro is None:
        unavailable.append({"mode": "metro", "label": "Namma Metro", "reason": "Metro data not loaded"})
    else:
        if metro_option:
            options.append(metro_option)
        else:
            unavailable.append({"mode": "metro", "label": "Namma Metro", "reason": metro_reason})

    options.sort(key=lambda o: rank_key(o, priority))
    cheapest = min(options, key=lambda o: rank_key(o, "cheapest"))
    fastest = min(options, key=lambda o: rank_key(o, "fastest"))
    for rank, option in enumerate(options, start=1):
        option["rank"] = rank
        option["id"] = option["mode"]
        option["is_cheapest"] = option is cheapest
        option["is_fastest"] = option is fastest

    return {
        "origin": origin, "destination": destination,
        "generated_at": datetime.now(IST).isoformat(timespec="seconds"),
        "depart_at": depart.isoformat(timespec="minutes"),
        "priority": priority,
        "road": {"distance_km": km, "duration_min": minutes, "polyline": road["polyline"]},
        "options": options, "unavailable": unavailable,
        "cheapest_mode": cheapest["mode"], "fastest_mode": fastest["mode"],
        "notes": [
            "Uber, Ola, Rapido and Namma Yatri do not share live prices with other apps; "
            "road fares are estimates from published rates.",
            "Road times are live from Ola Maps. Metro times use the real BMRCL timetable (Namma Metro GTFS, ODbL).",
        ],
    }
