#!/usr/bin/env python3
import argparse
import plistlib
from pathlib import Path


ICON_SIZES = (120, 152, 167)


def load_icon_names(plist_path):
    """Return (name, image) pairs for CFBundleAlternateIcons.

    Each icon is listed under its own name. An icon's "aliases" are names that
    used to exist (the alternates inherited from upstream iSH). iOS remembers
    the chosen alternate by name, so a retired name stays registered and shows
    the icon that replaced it: the home screen moves to the new art with no
    prompt, and the picker maps the alias back to the icon it now shows.
    """
    with open(plist_path, "rb") as plist:
        icons = plistlib.load(plist)
    entries = []
    for name in sorted(icons.keys()):
        if not name:
            continue
        entries.append((name, name))
        entries.extend((alias, name) for alias in icons[name].get("aliases", []))
    names = [name for name, _ in entries]
    duplicates = sorted({name for name in names if names.count(name) > 1})
    if duplicates:
        raise SystemExit(f"duplicate alternate icon names: {', '.join(duplicates)}")
    return sorted(entries)


def write_info_header(icon_names, header_path):
    def icon_entry(entry):
        name, image = entry
        files = "".join(f"<string>{image}-{size}</string>" for size in ICON_SIZES)
        return (
            f"<key>{name}</key>"
            "<dict>"
            "<key>CFBundleIconFiles</key>"
            f"<array>{files}</array>"
            "</dict>"
        )

    alt_icons = "<key>CFBundleAlternateIcons</key><dict>"
    alt_icons += "".join(icon_entry(name) for name in icon_names)
    alt_icons += "</dict>"
    icon_stuff = (
        "</string>"
        f"<key>CFBundleIcons</key><dict>{alt_icons}</dict>"
        f"<key>CFBundleIcons~ipad</key><dict>{alt_icons}</dict>"
        "<key>kxcode</key><string>"
    )

    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_text("#define ICON_STUFF " + icon_stuff, encoding="utf-8")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--icons-plist", required=True, type=Path)
    parser.add_argument("--info-header", type=Path)
    parser.add_argument("--icons-dir", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--resources-dir", type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()

    icon_names = load_icon_names(args.icons_plist)
    if args.info_header is not None:
        write_info_header(icon_names, args.info_header)


if __name__ == "__main__":
    main()
