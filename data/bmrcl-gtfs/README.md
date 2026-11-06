# Namma Metro GTFS data

`bmrcl.zip` is the Namma Metro (BMRCL) GTFS feed from
https://github.com/Vonter/bmrcl-gtfs (file `gtfs/bmrcl.zip`, feed version 2026-08-17).

- Licence: Open Database License (ODbL 1.0). Attribution: BMRCL and Vonter.
- Built from BMRCL timetables and OpenStreetMap. Times between stations are
  estimates (a couple of minutes' difference from real operations is possible).
- Used by `src/live/metro.py` for stations, line order, distances, travel times,
  operating hours and the official station-to-station fare table.

To update: download the latest `gtfs/bmrcl.zip` from the repository above and
replace this file.

## Pink Line (added by this project)

The upstream feed has no Pink Line yet. `tools/add_pink_line.py` adds its first section,
Kalena Agrahara - Tavarekere (Kalena Agrahara, Hulimavu, IIMB, JP Nagar 4th Phase,
Jayadeva Hospital, Tavarekere; about 7.5 km), which opens on 13 Nov 2026:

- Stations: OpenStreetMap coordinates (ODbL). Jayadeva Hospital is the existing Yellow
  Line station and becomes an interchange (2 minute transfer).
- Service: its own services (`PINK_weekday` ...) starting 20261113, with the same weekdays
  and holiday exceptions as the other lines, so no trip before the opening uses it.
- Timetable: estimated until BMRCL publishes one (every 10 minutes, 06:00-23:00,
  about 34 km/h between stations with a 30 s stop).
- Fares: BMRCL distance slabs, anchored on the official fares from Jayadeva Hospital
  (every Pink Line trip to the rest of the network passes it). Not official until BMRCL
  publishes a Pink Line fare table.

After replacing `bmrcl.zip` with a newer upstream feed, run
`python3 tools/add_pink_line.py` again (it is safe to run more than once). Once the
upstream feed includes the Pink Line, stop running it.

