"""
Search history stored in SQLite.

SQLite is a file-based database built into Python (`sqlite3`), so there is no
database server to install. The database is a single file (default:
travel_history.db in the project folder).
"""

import sqlite3
from datetime import datetime, timezone

SCHEMA = """
CREATE TABLE IF NOT EXISTS searches (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    source        TEXT NOT NULL,
    destination   TEXT NOT NULL,
    cheapest_mode TEXT,              -- NULL when no mode is available
    cheapest_fare REAL,
    searched_at   TEXT NOT NULL      -- ISO 8601 timestamp (UTC)
)
"""

LIVE_SCHEMA = """
CREATE TABLE IF NOT EXISTS live_searches (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    origin_name   TEXT NOT NULL,
    origin_lat    REAL NOT NULL,
    origin_lng    REAL NOT NULL,
    dest_name     TEXT NOT NULL,
    dest_lat      REAL NOT NULL,
    dest_lng      REAL NOT NULL,
    cheapest_mode TEXT,
    cheapest_fare REAL,
    searched_at   TEXT NOT NULL
)
"""


class HistoryStore:
    """Small data-access layer: the rest of the app never writes SQL itself."""

    def __init__(self, db_path):
        self.db_path = str(db_path)
        self._run(SCHEMA)
        self._run(LIVE_SCHEMA)

    def _run(self, sql, params=()):
        """Execute one SQL statement and return its rows as dictionaries.

        A fresh connection per call is simple and safe when the web server
        handles requests on several threads. `with conn` commits the change.
        """
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        try:
            with conn:
                rows = conn.execute(sql, params).fetchall()
            return [dict(row) for row in rows]
        finally:
            conn.close()

    def add(self, source, destination, cheapest_mode=None, cheapest_fare=None):
        timestamp = datetime.now(timezone.utc).isoformat(timespec="seconds")
        # "?" placeholders prevent SQL injection.
        self._run(
            "INSERT INTO searches (source, destination, cheapest_mode, cheapest_fare, searched_at) "
            "VALUES (?, ?, ?, ?, ?)",
            (source, destination, cheapest_mode, cheapest_fare, timestamp),
        )

    def recent(self, limit=10):
        return self._run(
            "SELECT source, destination, cheapest_mode, cheapest_fare, searched_at "
            "FROM searches ORDER BY id DESC LIMIT ?",
            (limit,),
        )

    def popular(self, limit=5):
        return self._run(
            "SELECT source, destination, COUNT(*) AS searches "
            "FROM searches GROUP BY source, destination "
            "ORDER BY searches DESC, MAX(id) DESC LIMIT ?",
            (limit,),
        )

    def clear(self):
        self._run("DELETE FROM searches")
        self._run("DELETE FROM live_searches")

    # ---- live mode (real places, stored with coordinates so they can be re-run) ----
    def add_live(self, origin, destination, cheapest_mode=None, cheapest_fare=None):
        timestamp = datetime.now(timezone.utc).isoformat(timespec="seconds")
        self._run(
            "INSERT INTO live_searches (origin_name, origin_lat, origin_lng, dest_name, dest_lat, dest_lng, "
            "cheapest_mode, cheapest_fare, searched_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (origin["name"], origin["lat"], origin["lng"], destination["name"], destination["lat"],
             destination["lng"], cheapest_mode, cheapest_fare, timestamp),
        )

    def recent_live(self, limit=6):
        rows = self._run(
            "SELECT origin_name, origin_lat, origin_lng, dest_name, dest_lat, dest_lng, cheapest_mode, "
            "cheapest_fare, searched_at FROM live_searches ORDER BY id DESC LIMIT ?",
            (limit,),
        )
        return [
            {"origin": {"name": r["origin_name"], "lat": r["origin_lat"], "lng": r["origin_lng"]},
             "destination": {"name": r["dest_name"], "lat": r["dest_lat"], "lng": r["dest_lng"]},
             "cheapest_mode": r["cheapest_mode"], "cheapest_fare": r["cheapest_fare"],
             "searched_at": r["searched_at"]}
            for r in rows
        ]
