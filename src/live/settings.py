"""
Settings for live mode, read from environment variables or a local `.env` file.

The Ola Maps API key stays on the server. It is never sent to the browser.
"""

import os
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parents[2]
ENV_FILE = PROJECT_ROOT / ".env"
METRO_GTFS_ZIP = PROJECT_ROOT / "data" / "bmrcl-gtfs" / "bmrcl.zip"

# Bengaluru city centre, used to bias place search results.
CITY_CENTRE = (12.9716, 77.5946)


def load_env_file(path=ENV_FILE):
    """Read KEY=VALUE lines from .env into os.environ (existing variables win)."""
    path = Path(path)
    if not path.exists():
        return
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


def ola_maps_api_key():
    load_env_file()
    return os.environ.get("OLA_MAPS_API_KEY", "").strip() or None
