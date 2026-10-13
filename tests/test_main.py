"""Smoke tests for the terminal menu, driven by scripted input."""

from src.main import format_minutes, format_rupees, run


def scripted(*answers):
    answers = iter(answers)
    return lambda prompt="": next(answers)


def test_find_routes_flow(capsys):
    run(scripted("2", "college", "12", "5"))     # name + number input
    out = capsys.readouterr().out
    assert "College  ->  Airport" in out
    assert "Recommendation (cheapest): Bus at ₹57" in out
    assert "Fastest option           : Car Taxi (quicker route) in 43 min" in out


def test_invalid_inputs_are_handled(capsys):
    run(scripted("9", "2", "Mars", "99", "Hospital", "Airport", "5"))
    out = capsys.readouterr().out
    assert "Invalid choice" in out
    assert "Unknown location: Mars" in out
    assert "Please enter a number from 1 to 12" in out
    assert "METRO  [NOT AVAILABLE]" in out


def test_same_location_message(capsys):
    run(scripted("2", "Market", "market", "5"))
    assert "no journey is needed" in capsys.readouterr().out


def test_other_menu_options(capsys):
    run(scripted("1", "3", "4", "5"))
    out = capsys.readouterr().out
    assert "AVAILABLE LOCATIONS" in out
    assert "charged once per journey" in out
    assert "Metro (6 connections)" in out
    assert "Goodbye" in out


def test_formatters():
    assert format_rupees(57) == "₹57"
    assert format_rupees(17.5) == "₹17.50"
    assert format_minutes(45) == "45 min"
    assert format_minutes(88) == "1 h 28 min"
    assert format_minutes(120) == "2 h"
