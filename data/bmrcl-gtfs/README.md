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
