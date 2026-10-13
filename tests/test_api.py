"""Tests for the FastAPI backend and the SQLite history store."""

import pytest

pytest.importorskip("fastapi")
pytest.importorskip("httpx2")

from fastapi.testclient import TestClient  # noqa: E402

from src.history import HistoryStore  # noqa: E402
from src.server import create_app  # noqa: E402


@pytest.fixture
def client(tmp_path):
    # Each test gets its own empty database file.
    return TestClient(create_app(db_path=tmp_path / "test.db", maps_client=None, metro=None))


def test_health(client):
    body = client.get("/api/health").json()
    assert body["status"] == "ok" and body["locations"] == 12 and body["connections"] == 47


def test_locations_have_coordinates(client):
    locations = client.get("/api/locations").json()
    assert len(locations) == 12
    assert {"name", "x", "y"} <= set(locations[0])


def test_fares_and_network(client):
    fares = client.get("/api/fares").json()
    assert [f["mode"] for f in fares] == ["bus", "metro", "bike_taxi", "car_taxi"]
    assert len(client.get("/api/network").json()) == 47


def test_routes_college_to_airport(client):
    response = client.get("/api/routes", params={"source": "College", "destination": "Airport"})
    assert response.status_code == 200
    body = response.json()
    cheapest_rows = [(o["mode"], o["fare"]) for o in body["options"] if o["variant"] == "cheapest"]
    assert cheapest_rows == [("bus", 57), ("metro", 91), ("bike_taxi", 212), ("car_taxi", 390)]
    # The car has a quicker (longer) route: it is listed too and is the single fastest.
    assert body["cheapest_mode"] == "bus"
    assert body["fastest_mode"] == "car_taxi_fastest"
    assert sum(o["is_fastest"] for o in body["options"]) == 1
    assert sum(o["is_cheapest"] for o in body["options"]) == 1
    bus = body["options"][0]
    assert bus["is_cheapest"] and not bus["is_fastest"]
    assert bus["path"][0] == "College" and bus["path"][-1] == "Airport"
    assert sum(leg["distance_km"] for leg in bus["legs"]) == bus["distance_km"]
    assert sum(leg["time_min"] for leg in bus["legs"]) == bus["time_min"]


def test_routes_accept_any_letter_case(client):
    body = client.get("/api/routes", params={"source": "college", "destination": " AIRPORT "}).json()
    assert body["source"] == "College" and body["destination"] == "Airport"


def test_routes_report_unavailable_mode(client):
    body = client.get("/api/routes", params={"source": "Hospital", "destination": "Airport"}).json()
    assert body["unavailable"] == [{"mode": "metro", "label": "Metro"}]


def test_routes_unknown_location_returns_404(client):
    response = client.get("/api/routes", params={"source": "Mars", "destination": "Airport"})
    assert response.status_code == 404
    assert "Unknown location" in response.json()["detail"]


def test_routes_missing_parameter_returns_422(client):
    assert client.get("/api/routes", params={"source": "College"}).status_code == 422


def test_routes_same_location(client):
    body = client.get("/api/routes", params={"source": "Market", "destination": "Market"}).json()
    assert body["same_location"] is True
    assert body["options"] == []


def test_history_records_searches_and_can_be_cleared(client):
    client.get("/api/routes", params={"source": "College", "destination": "Airport"})
    client.get("/api/routes", params={"source": "Hospital", "destination": "Airport"})
    client.get("/api/routes", params={"source": "College", "destination": "Airport"})
    client.get("/api/routes", params={"source": "Market", "destination": "Market"})  # not saved

    history = client.get("/api/history").json()
    assert [(h["source"], h["destination"]) for h in history["recent"]] == [
        ("College", "Airport"), ("Hospital", "Airport"), ("College", "Airport"),
    ]
    assert history["recent"][0]["cheapest_mode"] == "bus"
    assert history["popular"][0] == {"source": "College", "destination": "Airport", "searches": 2}

    assert client.delete("/api/history").status_code == 403          # needs the custom header
    headers = {"X-Requested-With": "smart-travel"}
    assert client.delete("/api/history", headers=headers).json() == {"cleared": True}
    assert client.get("/api/history").json() == {"recent": [], "popular": []}


def test_history_store_uses_parameters_safely(tmp_path):
    store = HistoryStore(tmp_path / "h.db")
    tricky = "x'); DROP TABLE searches; --"
    store.add(tricky, "Airport")
    assert store.recent()[0]["source"] == tricky


def test_frontend_is_served(client):
    page = client.get("/")
    assert page.status_code == 200
    assert "Smart Travel Cost Finder" in page.text
    assert client.get("/app.js").status_code == 200
    assert client.get("/styles.css").status_code == 200
