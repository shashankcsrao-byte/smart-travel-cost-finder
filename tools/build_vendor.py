"""
Copy third-party assets into web/vendor so the site works with no internet.

Run once after `npm install` in tools/vendor:
    cd tools/vendor && npm install
    python tools/build_vendor.py

Produces:
    web/vendor/leaflet/      leaflet.js, leaflet.css, images/   (BSD-2-Clause)
    web/vendor/fonts/        Outfit + Geist latin woff2 files    (SIL OFL 1.1)
    web/vendor/icons.svg     Material Symbols Rounded sprite     (Apache 2.0)
"""

import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NODE = ROOT / "tools" / "vendor" / "node_modules"
OUT = ROOT / "web" / "vendor"

ICONS = """
alt_route arrow_forward bolt check_circle close content_copy directions_bus directions_walk
electric_rickshaw home info link local_taxi location_on monitoring my_location near_me
open_in_new pause payments pin_drop play_arrow refresh route savings schedule search share
skip_next skip_previous star subway swap_vert timeline trip_origin two_wheeler verified
warning wifi_off work
""".split()

FONTS = {
    "outfit": ["500", "600", "700", "800"],
    "geist": ["400", "500", "600"],
}


def build_sprite():
    src = NODE / "@material-symbols" / "svg-400" / "rounded"
    symbols = []
    for name in ICONS:
        for variant, suffix in (("o", ""), ("f", "-fill")):
            path = src / f"{name}{suffix}.svg"
            if not path.exists():
                path = src / f"{name}.svg"          # some icons have no filled variant
            svg = path.read_text(encoding="utf-8")
            view_box = re.search(r'viewBox="([^"]+)"', svg).group(1)
            inner = re.sub(r"^.*?<svg[^>]*>|</svg>\s*$", "", svg, flags=re.S).strip()
            symbols.append(f'<symbol id="{variant}-{name}" viewBox="{view_box}">{inner}</symbol>')
    sprite = ('<svg xmlns="http://www.w3.org/2000/svg" style="display:none">'
              "<!-- Material Symbols Rounded (Apache License 2.0), Google -->"
              + "".join(symbols) + "</svg>\n")
    (OUT / "icons.svg").write_text(sprite, encoding="utf-8")
    return len(symbols)


def copy_fonts():
    dest = OUT / "fonts"
    dest.mkdir(parents=True, exist_ok=True)
    faces = []
    for family, weights in FONTS.items():
        for w in weights:
            name = f"{family}-latin-{w}-normal.woff2"
            shutil.copy2(NODE / "@fontsource" / family / "files" / name, dest / name)
            css_family = "Outfit" if family == "outfit" else "Geist"
            faces.append(
                f'@font-face{{font-family:"{css_family}";font-style:normal;font-weight:{w};'
                f'font-display:swap;src:url("fonts/{name}") format("woff2");}}'
            )
    (OUT / "fonts.css").write_text("/* Outfit and Geist, SIL Open Font License 1.1 */\n" + "\n".join(faces) + "\n")
    return len(faces)


def copy_leaflet():
    dest = OUT / "leaflet"
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    dist = NODE / "leaflet" / "dist"
    for name in ("leaflet.js", "leaflet.css"):
        shutil.copy2(dist / name, dest / name)
    shutil.copytree(dist / "images", dest / "images")
    shutil.copy2(NODE / "leaflet" / "LICENSE", dest / "LICENSE")


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    n_icons = build_sprite()
    n_fonts = copy_fonts()
    copy_leaflet()
    print(f"icons: {n_icons} symbols, fonts: {n_fonts} faces, leaflet copied -> {OUT}")
