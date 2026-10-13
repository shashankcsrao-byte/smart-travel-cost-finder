"""
Check that live mode works with your Ola Maps key.

    python -m src.live.check

Makes three small API calls (place search, directions, distance matrix) and
prints what the app understood from each one.
"""

import sys

from .olamaps import OlaMapsClient, OlaMapsError
from .settings import ola_maps_api_key

MG_ROAD = (12.9756, 77.6066)
INDIRANAGAR = (12.9784, 77.6408)


def main():
    key = ola_maps_api_key()
    if not key:
        print("No key found. Copy .env.example to .env and set OLA_MAPS_API_KEY.")
        return 1
    client = OlaMapsClient(key)
    ok = True

    for title, call in [
        ("Place search 'Koramangala'", lambda: client.autocomplete("Koramangala")[:3]),
        ("Directions MG Road -> Indiranagar", lambda: client.directions(MG_ROAD, INDIRANAGAR)),
        ("Distance matrix MG Road -> Indiranagar", lambda: client.distance_matrix([MG_ROAD], [INDIRANAGAR])),
    ]:
        try:
            result = call()
            print(f"OK    {title}\n      {result}")
            if not result:
                ok = False
                print("      (empty result - please share this output)")
        except OlaMapsError as error:
            ok = False
            print(f"FAIL  {title}\n      {error}")

    print("\nLive mode is ready." if ok else "\nSomething failed - share this output to get it fixed.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
