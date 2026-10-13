"""
Terminal interface for Smart Travel Cost Finder.

Run from the project folder:
    python3 -m src.main
"""

import sys

from .fares import FARE_RULES, mode_label
from .graph import CONNECTIONS, MODES, build_graph, list_connections
from .routing import UnknownLocationError, compare_routes, find_location

LINE = "=" * 64
THIN = "-" * 64
DISCLAIMER = "  Note: sample city data and illustrative fares - not live or official values."


# ---------------------------------------------------------------------------
# Formatting helpers (also reused by the Streamlit app)
# ---------------------------------------------------------------------------

def format_rupees(amount):
    """57 -> '₹57', 57.5 -> '₹57.50'."""
    if float(amount).is_integer():
        return f"₹{int(amount)}"
    return f"₹{amount:.2f}"


def format_km(km):
    return f"{int(km)} km" if float(km).is_integer() else f"{km:.1f} km"


def format_minutes(minutes):
    """45 -> '45 min', 88 -> '1 h 28 min'."""
    minutes = int(round(minutes))
    hours, mins = divmod(minutes, 60)
    if hours == 0:
        return f"{mins} min"
    return f"{hours} h {mins} min" if mins else f"{hours} h"


def format_path(path):
    return " -> ".join(path)


# ---------------------------------------------------------------------------
# Menu actions
# ---------------------------------------------------------------------------

def print_locations(graph):
    print(f"\n{LINE}\n AVAILABLE LOCATIONS\n{LINE}")
    for number, name in enumerate(graph, start=1):
        print(f"  {number:>2}. {name}")


def print_fare_rules():
    print(f"\n{LINE}\n FARE RULES (illustrative sample values)\n{LINE}")
    print(f"  {'Mode':<12}{'Base fare':>12}{'Per km':>10}")
    print(f"  {THIN[:34]}")
    for mode in MODES:
        rule = FARE_RULES[mode]
        print(f"  {rule['label']:<12}{format_rupees(rule['base']):>12}{format_rupees(rule['per_km']):>10}")
    print("\n  Total fare = base fare + (per-km rate x total route distance)")
    print("  The base fare is charged once per journey, not once per road segment.")


def print_connections():
    print(f"\n{LINE}\n TRANSPORT CONNECTIONS (all bidirectional)\n{LINE}")
    for mode in MODES:
        rows = list_connections(CONNECTIONS, mode)
        print(f"\n  {mode_label(mode)} ({len(rows)} connections)")
        print(f"  {THIN[:60]}")
        for a, b, _, km, minutes in rows:
            print(f"  {a + ' <-> ' + b:<40}{format_km(km):>8}{format_minutes(minutes):>12}")


def print_comparison(comparison):
    print(f"\n{LINE}\n {comparison.source}  ->  {comparison.destination}\n{LINE}")

    if comparison.same_location:
        print("  Source and destination are the same - no journey is needed.")
        return

    if not comparison.options:
        print("  No transport mode can reach this destination in the sample network.")
        return

    cheapest_id = comparison.cheapest.id      # exactly one cheapest
    fastest_id = comparison.fastest.id        # exactly one fastest

    for rank, option in enumerate(comparison.options, start=1):
        tags = []
        if option.id == cheapest_id:
            tags.append("CHEAPEST")
        if option.id == fastest_id:
            tags.append("FASTEST")
        tag_text = f"  [{' | '.join(tags)}]" if tags else ""

        print(f"\n  #{rank}  {option.label.upper()}{tag_text}")
        print(f"      Fare     : {format_rupees(option.fare)}")
        print(f"      Distance : {format_km(option.distance_km)}")
        print(f"      Time     : {format_minutes(option.time_min)}")
        print(f"      Route    : {format_path(option.path)}")

    for mode in comparison.unavailable:
        print(f"\n  --  {mode_label(mode).upper()}  [NOT AVAILABLE]")
        print("      No route between these locations using this mode only.")

    cheapest = comparison.cheapest
    fastest = comparison.fastest
    print(f"\n{THIN}")
    print(f"  Recommendation (cheapest): {cheapest.label} at {format_rupees(cheapest.fare)}")
    if len(comparison.tied_cheapest) > 1:
        others = ", ".join(o.label for o in comparison.tied_cheapest if o is not cheapest)
        print(f"    (same fare also by: {others})")
    print(f"  Fastest option           : {fastest.label} in {format_minutes(fastest.time_min)}")
    print("  Times exclude waiting, walking and live traffic.")
    print(DISCLAIMER)


def ask_location(prompt, graph, input_func):
    """Ask until a valid location is given (number or name). Returns None to cancel."""
    names = list(graph)
    while True:
        text = input_func(prompt).strip()
        if text.lower() in ("", "q", "back"):
            return None
        if text.isdigit():
            number = int(text)
            if 1 <= number <= len(names):
                return names[number - 1]
            print(f"  Please enter a number from 1 to {len(names)}, or a location name.")
            continue
        try:
            return find_location(graph, text)
        except UnknownLocationError as error:
            print(f"  {error}. Type a number or a name from the list (q to go back).")


def find_routes(graph, input_func):
    print_locations(graph)
    print("\n  Enter a number or a location name (blank or q to go back).")
    source = ask_location("  Source      : ", graph, input_func)
    if source is None:
        return
    destination = ask_location("  Destination : ", graph, input_func)
    if destination is None:
        return
    print_comparison(compare_routes(graph, source, destination))


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

MENU = f"""
{LINE}
 SMART TRAVEL COST FINDER
 Graph-Based Multi-Modal Transport Cost Optimization
{LINE}
  1. View available locations
  2. Find cheapest routes
  3. View fare rules
  4. View transport connections
  5. Exit"""


def run(input_func=input):
    graph = build_graph()
    while True:
        print(MENU)
        choice = input_func("\n  Choose an option (1-5): ").strip()
        if choice == "1":
            print_locations(graph)
        elif choice == "2":
            find_routes(graph, input_func)
        elif choice == "3":
            print_fare_rules()
        elif choice == "4":
            print_connections()
        elif choice == "5":
            print("\n  Thank you for using Smart Travel Cost Finder. Goodbye!")
            return
        else:
            print("  Invalid choice. Please enter a number from 1 to 5.")


def main():
    # Make the ₹ symbol print correctly on Windows terminals too.
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass
    try:
        run()
    except (KeyboardInterrupt, EOFError):
        print("\n  Exiting. Goodbye!")


if __name__ == "__main__":
    main()
