"""
Live trip planner for Bengaluru.

For one origin and destination it builds an option per transport type:

    Auto       live road route (Ola Maps) + Govt auto meter fare
    Cab        live road route (Ola Maps) + Govt-notified cab fare
    Bike taxi  live road route (Ola Maps) + indicative fare
    Bus        live road time x slowdown + BMTC stage fare (estimate)
    Metro      walk/auto to a station + OUR Dijkstra on the real metro graph
               + official BMRCL fare + walk/auto from the exit station

Options are ranked by fare (then time), exactly like the offline version.
"""

import json
from datetime import datetime
from urllib.parse import urlencode

from . import fares as F
from .metro import IST, haversine_km
from .olamaps import OlaMapsError

MODE_ORDER = ("bus", "metro", "auto", "bike_taxi", "cab")


# ---------------------------------------------------------------- deep links

def uber_link(origin, destination):
    """Uber universal link with pickup and drop pre-filled (developer.uber.com deep links)."""
    place = lambda p: json.dumps({"latitude": p["lat"], "longitude": p["lng"],
                                  "addressLine1": p["name"]}, separators=(",", ":"))
    return "https://m.uber.com/looking?" + urlencode({"pickup": place(origin), "drop[0]": place(destination)})


def maps_transit_link(origin, destination):
    """Google Maps URL (no API key needed) showing public-transport directions."""
    return "https://www.google.com/maps/dir/?" + urlencode({
        "api": 1, "origin": f"{origin['lat']},{origin['lng']}",
        "destination": f"{destination['lat']},{destination['lng']}", "travelmode": "transit"})


APP_LINKS = {
    "namma_yatri": {"label": "Namma Yatri", "url": "https://nammayatri.in/"},
    "ola": {"label": "Ola", "url": "https://www.olacabs.com/"},
    "rapido": {"label": "Rapido", "url": "https://www.rapido.bike/"},
}


# ------------------------------------------------------------------- helpers

def _leg(kind, start, end, km, minutes, **extra):
    return {"kind": kind, "from": start, "to": end,
            "distance_km": round(km, 2), "time_min": round(minutes, 1), **extra}


def _access_leg(kind_name, place_name, station, road, hour, towards_station):
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

def plan_metro(maps, metro, origin, destination, now):
    """Best (cheapest, then quickest) metro journey, or (None, reason)."""
    if not metro.is_running(now):
        first, last = metro.hours(now) or ("?", "?")
        return None, f"Metro is not running now (trains run about {first}–{last})"

    o_point, d_point = (origin["lat"], origin["lng"]), (destination["lat"], destination["lng"])
    starts = metro.nearest_stations(o_point, k=3)
    ends = metro.nearest_stations(d_point, k=3)
    if not starts or not ends:
        return None, "No metro station within 6 km of the start or the destination"
    if starts[0][0]["id"] == ends[0][0]["id"]:
        return None, f"Both places are closest to the same station ({starts[0][0]['name']}), so the metro does not help"

    station_point = lambda s: (s["lat"], s["lng"])
    approximate = False
    try:
        to_station = maps.distance_matrix([o_point], [station_point(s) for s, _ in starts])[0]
        from_station = [row[0] for row in maps.distance_matrix([station_point(s) for s, _ in ends], [d_point])]
    except OlaMapsError:
        approximate = True
        to_station = [None] * len(starts)
        from_station = [None] * len(ends)

    hour = now.astimezone(IST).hour
    best = None
    for (a, _), road_a in zip(starts, to_station):
        road_a = road_a or _approx_road(o_point, station_point(a))
        for (b, _), road_b in zip(ends, from_station):
            road_b = road_b or _approx_road(station_point(b), d_point)
            if a["id"] == b["id"]:
                continue
            journey = metro.route(a["id"], b["id"])
            fare_info = metro.fare(a["id"], b["id"], now)
            if journey is None or fare_info["token"] is None:
                continue
            first_leg, first_fare = _access_leg("access", origin["name"], a, road_a, hour, True)
            last_leg, last_fare = _access_leg("egress", destination["name"], b, road_b, hour, False)
            total_fare = fare_info["token"] + first_fare + last_fare
            total_time = first_leg["time_min"] + journey["time_min"] + last_leg["time_min"]
            candidate = (total_fare, total_time, first_leg, journey, fare_info, last_leg)
            if best is None or candidate[:2] < best[:2]:
                best = candidate

    if best is None:
        return None, "The start and destination are closest to the same metro station"

    total_fare, total_time, first_leg, journey, fare_info, last_leg = best
    legs = [first_leg]
    for ride in journey["rides"]:
        legs.append(_leg("metro", ride["from"], ride["to"], ride["distance_km"], ride["time_min"],
                         line=ride["line_name"], color=ride["color"], stops=ride["stops"], coords=ride["coords"],
                         from_id=ride["stations"][0], to_id=ride["stations"][-1]))
    legs.append(last_leg)

    notes = [f"Metro time includes about {round(journey['time_min'] - sum(r['time_min'] for r in journey['rides']))} min "
             f"of average waiting{' and ' + str(journey['changes']) + ' line change(s)' if journey['changes'] else ''}."]
    if fare_info["smart_card"]:
        notes.append(f"Smart card: ₹{fare_info['smart_card']:g} ({fare_info['timeframe']}) instead of ₹{fare_info['token']:g}.")
    if first_leg["kind"] == "auto" or last_leg["kind"] == "auto":
        notes.append("Includes auto fare (meter rate) to/from the station.")
    if approximate:
        notes.append("Station access distances are approximate (map service unavailable).")

    distance = sum(leg["distance_km"] for leg in legs)
    return {
        "mode": "metro", "label": "Namma Metro", "providers": "BMRCL",
        "fare": round(total_fare), "fare_kind": "official",
        "fare_text": f"₹{fare_info['token']:g} metro" + (f" + ₹{first_leg.get('fare', 0) + last_leg.get('fare', 0)} auto"
                                                         if first_leg["kind"] == "auto" or last_leg["kind"] == "auto" else ""),
        "fare_source": "Official BMRCL station-to-station fare (Namma Metro GTFS)",
        "distance_km": round(distance, 2), "time_min": round(total_time, 1),
        "legs": legs, "notes": notes,
        "links": [{"label": "Directions in Google Maps", "url": maps_transit_link(origin, destination)}],
    }, None


# --------------------------------------------------------------------- plan

def plan_trip(maps, metro, origin, destination, now=None):
    now = now or datetime.now(IST)
    hour = now.astimezone(IST).hour
    road = maps.directions((origin["lat"], origin["lng"]), (destination["lat"], destination["lng"]))
    km, minutes = road["distance_km"], road["duration_min"]
    road_leg = _leg("road", origin["name"], destination["name"], km, minutes, polyline=road["polyline"])

    options, unavailable = [], []

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
        "links": [{"label": "Bus routes in Google Maps", "url": maps_transit_link(origin, destination)}],
    })

    if metro is None:
        unavailable.append({"mode": "metro", "label": "Namma Metro", "reason": "Metro data not loaded"})
    else:
        metro_option, reason = plan_metro(maps, metro, origin, destination, now)
        if metro_option:
            options.append(metro_option)
        else:
            unavailable.append({"mode": "metro", "label": "Namma Metro", "reason": reason})

    options.sort(key=lambda o: (o["fare"], o["time_min"], MODE_ORDER.index(o["mode"])))
    best_fare = options[0]["fare"]
    best_time = min(o["time_min"] for o in options)
    fastest = min(options, key=lambda o: (o["time_min"], o["fare"], MODE_ORDER.index(o["mode"])))
    for rank, option in enumerate(options, start=1):
        option["rank"] = rank
        option["is_cheapest"] = option["fare"] == best_fare
        option["is_fastest"] = option["time_min"] == best_time

    return {
        "origin": origin, "destination": destination,
        "generated_at": now.astimezone(IST).isoformat(timespec="seconds"),
        "road": {"distance_km": km, "duration_min": minutes, "polyline": road["polyline"]},
        "options": options, "unavailable": unavailable,
        "cheapest_mode": options[0]["mode"], "fastest_mode": fastest["mode"],
        "notes": [
            "Uber, Ola, Rapido and Namma Yatri do not share live prices with other apps; "
            "road fares are estimates from published rates.",
            "Road times are live from Ola Maps. Metro data: BMRCL via Namma Metro GTFS (ODbL).",
        ],
    }
