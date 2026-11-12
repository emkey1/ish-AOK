#!/usr/bin/env python3
"""Generate the iSH-AOK alternate icon SVGs (1024x1024, full bleed). See README.md."""
import sys
from pathlib import Path

OUT = Path(sys.argv[1])
OUT.mkdir(parents=True, exist_ok=True)

MONO = "ui-monospace, 'SF Mono', Menlo, monospace"
SANS = "-apple-system, 'SF Pro Display', 'Helvetica Neue', sans-serif"
ROUND = "ui-rounded, 'SF Pro Rounded', -apple-system, sans-serif"


def svg(body, defs=""):
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" '
        'viewBox="0 0 1024 1024">'
        f"<defs>{defs}</defs>{body}</svg>"
    )


ICONS = {}

# 1. Prompt: the AOK teal with a green prompt and block cursor.
ICONS["aok-prompt"] = ("Prompt", svg(
    f"""
<rect width="1024" height="1024" fill="url(#g)"/>
<text x="150" y="590" font-family="{MONO}" font-weight="800" font-size="330" fill="#4ade80">❯</text>
<text x="380" y="590" font-family="{MONO}" font-weight="700" font-size="250" fill="#f1f5f9" letter-spacing="-6">aok</text>
<rect x="390" y="650" width="150" height="40" rx="6" fill="#4ade80"/>
""",
    '<linearGradient id="g" x1="0" y1="0" x2="1" y2="1">'
    '<stop offset="0" stop-color="#0d1f26"/><stop offset="1" stop-color="#173a46"/></linearGradient>',
))

# 2. Amber phosphor CRT.
ICONS["aok-amber"] = ("Amber Phosphor", svg(
    f"""
<rect width="1024" height="1024" fill="url(#bg)"/>
<g filter="url(#glow)" fill="#ffb000">
<text x="512" y="560" text-anchor="middle" font-family="{MONO}" font-weight="800" font-size="330">AOK</text>
<rect x="160" y="640" width="150" height="56"/>
</g>
<rect width="1024" height="1024" fill="url(#scan)"/>
<rect width="1024" height="1024" fill="url(#vig)"/>
""",
    '<radialGradient id="bg" cx="0.5" cy="0.5" r="0.7"><stop offset="0" stop-color="#2b1700"/>'
    '<stop offset="1" stop-color="#0a0500"/></radialGradient>'
    '<radialGradient id="vig" cx="0.5" cy="0.5" r="0.75"><stop offset="0.6" stop-color="#000" stop-opacity="0"/>'
    '<stop offset="1" stop-color="#000" stop-opacity="0.55"/></radialGradient>'
    '<pattern id="scan" width="8" height="8" patternUnits="userSpaceOnUse">'
    '<rect width="8" height="3" fill="#000" opacity="0.35"/></pattern>'
    '<filter id="glow" x="-20%" y="-20%" width="140%" height="140%">'
    '<feGaussianBlur stdDeviation="14" result="b"/>'
    '<feMerge><feMergeNode in="b"/><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>',
))

# 3. Green-screen boot log: every guest comes up [ OK ].
lines = [("arm64", 210), ("x86_64", 300), ("riscv64", 390)]
boot = "".join(
    f'<text x="120" y="{y}" font-family="{MONO}" font-weight="700" font-size="62" fill="#33ff66">'
    f'[ <tspan fill="#b6ffc8">OK</tspan> ] {name}</text>'
    for name, y in lines
)
ICONS["aok-boot"] = ("Boot Log", svg(
    f"""
<rect width="1024" height="1024" fill="#03140a"/>
<g filter="url(#glow)">
{boot}
<text x="120" y="760" font-family="{MONO}" font-weight="800" font-size="290" fill="#33ff66">AOK</text>
<rect x="700" y="560" width="130" height="200" fill="#33ff66"/>
</g>
<rect width="1024" height="1024" fill="url(#scan)"/>
""",
    '<pattern id="scan" width="8" height="8" patternUnits="userSpaceOnUse">'
    '<rect width="8" height="3" fill="#000" opacity="0.35"/></pattern>'
    '<filter id="glow" x="-20%" y="-20%" width="140%" height="140%">'
    '<feGaussianBlur stdDeviation="10" result="b"/>'
    '<feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge></filter>',
))

# 4. Three guests: fanned cards for arm64, x86-64, riscv64.
def card(x, y, rot, fill, label, big=None, where="tl"):
    pos = {"tl": 'x="40" y="90"',
           "tr": 'x="420" y="90" text-anchor="end"',
           "left": 'x="0" y="0" text-anchor="middle" transform="translate(72 300) rotate(-90)"'}[where]
    inner = (
        f'<text {pos} font-family="{MONO}" font-weight="700" font-size="58" fill="#fff" '
        f'opacity="0.95">{label}</text>'
    )
    if big:
        inner += (
            f'<text x="230" y="390" text-anchor="middle" font-family="{SANS}" font-weight="900" '
            f'font-size="190" fill="#fff" letter-spacing="-4">{big}</text>'
        )
    return (
        f'<g transform="translate({x} {y}) rotate({rot})">'
        f'<rect x="6" y="14" width="460" height="560" rx="56" fill="#000" opacity="0.18"/>'
        f'<rect width="460" height="560" rx="56" fill="{fill}"/>{inner}</g>'
    )

ICONS["aok-guests"] = ("Three Guests", svg(
    f"""
<rect width="1024" height="1024" fill="#f3efe6"/>
{card(150, 300, -14, "#d9a21b", "")}
{card(420, 150, 12, "#e86a2b", "")}
{card(282, 250, 0, "#0e3a45", "", "AOK")}
<text x="512" y="740" text-anchor="middle" font-family="{MONO}" font-weight="700" font-size="34" fill="#f3efe6" opacity="0.85">arm64 · x86 · rv64</text>
""",
))

# 5. Gadget chain: A -> O -> K tiles threaded like the AOK JIT's gadgets.
def tile(x, ch, fill):
    return (
        f'<rect x="{x}" y="372" width="220" height="280" rx="40" fill="{fill}"/>'
        f'<text x="{x+110}" y="590" text-anchor="middle" font-family="{MONO}" font-weight="800" '
        f'font-size="210" fill="#fff">{ch}</text>'
    )

ICONS["aok-gadgets"] = ("Gadget Chain", svg(
    f"""
<rect width="1024" height="1024" fill="url(#g)"/>
<path d="M 840 512 C 990 512 990 820 512 820 C 34 820 34 512 184 512" fill="none" stroke="#a78bfa"
 stroke-width="26" stroke-linecap="round" stroke-dasharray="2 46" opacity="0.9"/>
<path d="M 160 486 L 196 512 L 160 538" fill="none" stroke="#a78bfa" stroke-width="26" stroke-linecap="round" stroke-linejoin="round"/>
{tile(90, "A", "#7c3aed")}
<path d="M 318 512 h 58 m -20 -26 l 26 26 l -26 26" fill="none" stroke="#e9d5ff" stroke-width="22" stroke-linecap="round" stroke-linejoin="round"/>
{tile(402, "O", "#6d28d9")}
<path d="M 630 512 h 58 m -20 -26 l 26 26 l -26 26" fill="none" stroke="#e9d5ff" stroke-width="22" stroke-linecap="round" stroke-linejoin="round"/>
{tile(714, "K", "#5b21b6")}
<text x="512" y="250" text-anchor="middle" font-family="{MONO}" font-weight="600" font-size="64" fill="#c4b5fd">gadget → gadget</text>
""",
    '<linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1e1442"/>'
    '<stop offset="1" stop-color="#0c0820"/></linearGradient>',
))

# 6. Suspend to Disk: crescent moon, Zz, AOK.
stars = "".join(
    f'<circle cx="{x}" cy="{y}" r="{r}" fill="#fff" opacity="{o}"/>'
    for x, y, r, o in [(150, 160, 6, .8), (300, 110, 4, .6), (860, 180, 5, .7), (900, 420, 4, .5),
                       (120, 430, 4, .5), (760, 90, 3, .6), (210, 300, 3, .5), (640, 220, 3, .5)]
)
ICONS["aok-suspend"] = ("Suspend", svg(
    f"""
<rect width="1024" height="1024" fill="url(#sky)"/>
{stars}
<path d="M 600 180 A 230 230 0 1 0 760 560 A 190 190 0 1 1 600 180 Z" fill="#fde68a"/>
<text x="700" y="300" font-family="{ROUND}" font-weight="800" font-size="110" fill="#e0e7ff">Z</text>
<text x="790" y="220" font-family="{ROUND}" font-weight="800" font-size="72" fill="#e0e7ff" opacity="0.8">z</text>
<text x="512" y="880" text-anchor="middle" font-family="{SANS}" font-weight="800" font-size="200"
 fill="#e0e7ff" letter-spacing="24">AOK</text>
""",
    '<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#1e1b4b"/>'
    '<stop offset="1" stop-color="#3b2a7a"/></linearGradient>',
))

# 7. Wayland desktop: a window on a wallpaper with a dock.
ICONS["aok-desktop"] = ("Desktop", svg(
    f"""
<rect width="1024" height="1024" fill="url(#wall)"/>
<circle cx="820" cy="180" r="260" fill="#fff" opacity="0.08"/>
<rect x="120" y="190" width="784" height="560" rx="44" fill="#000" opacity="0.25" transform="translate(0 18)"/>
<rect x="120" y="190" width="784" height="560" rx="44" fill="#0f172a"/>
<path d="M 120 234 a 44 44 0 0 1 44 -44 h 696 a 44 44 0 0 1 44 44 v 46 h -784 Z" fill="#1e293b"/>
<circle cx="180" cy="236" r="16" fill="#f87171"/><circle cx="230" cy="236" r="16" fill="#fbbf24"/>
<circle cx="280" cy="236" r="16" fill="#4ade80"/>
<text x="180" y="430" font-family="{MONO}" font-weight="700" font-size="96" fill="#5eead4">❯</text>
<text x="270" y="430" font-family="{MONO}" font-weight="700" font-size="96" fill="#e2e8f0">aok</text>
<text x="180" y="560" font-family="{SANS}" font-weight="900" font-size="150" fill="#f8fafc" letter-spacing="-2">AOK</text>
<rect x="520" y="456" width="56" height="104" fill="#5eead4"/>
<rect x="272" y="828" width="480" height="104" rx="34" fill="#fff" opacity="0.28"/>
<rect x="300" y="848" width="64" height="64" rx="16" fill="#0f172a"/>
<rect x="390" y="848" width="64" height="64" rx="16" fill="#f472b6"/>
<rect x="480" y="848" width="64" height="64" rx="16" fill="#fbbf24"/>
<rect x="570" y="848" width="64" height="64" rx="16" fill="#60a5fa"/>
<rect x="660" y="848" width="64" height="64" rx="16" fill="#34d399"/>
""",
    '<linearGradient id="wall" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#0d9488"/>'
    '<stop offset="1" stop-color="#6d28d9"/></linearGradient>',
))

# 8. Blueprint: outlined AOK on a drafting grid with a dimension line.
ICONS["aok-blueprint"] = ("Blueprint", svg(
    f"""
<rect width="1024" height="1024" fill="#1b4f8f"/>
<rect width="1024" height="1024" fill="url(#minor)"/>
<rect width="1024" height="1024" fill="url(#major)"/>
<text x="512" y="600" text-anchor="middle" font-family="{SANS}" font-weight="900" font-size="320"
 fill="none" stroke="#f8fafc" stroke-width="10" stroke-linejoin="round" letter-spacing="6">AOK</text>
<g stroke="#bfdbfe" stroke-width="6" fill="none">
<path d="M 130 700 v 80 M 894 700 v 80 M 140 740 h 744"/>
<path d="M 170 722 l -30 18 l 30 18 M 854 722 l 30 18 l -30 18"/>
<path d="M 40 372 h 70 M 40 600 h 70 M 75 382 v 208"/>
</g>
<rect x="420" y="712" width="184" height="58" fill="#1b4f8f"/>
<text x="512" y="758" text-anchor="middle" font-family="{MONO}" font-size="48" fill="#bfdbfe">64-bit</text>
<text x="880" y="940" text-anchor="end" font-family="{MONO}" font-size="40" fill="#bfdbfe" opacity="0.8">REV AOK</text>
""",
    '<pattern id="minor" width="32" height="32" patternUnits="userSpaceOnUse">'
    '<path d="M 32 0 H 0 V 32" fill="none" stroke="#fff" stroke-opacity="0.08" stroke-width="2"/></pattern>'
    '<pattern id="major" width="128" height="128" patternUnits="userSpaceOnUse">'
    '<path d="M 128 0 H 0 V 128" fill="none" stroke="#fff" stroke-opacity="0.18" stroke-width="3"/></pattern>',
))

# 9. Silicon: a die with gold pins and traces.
pins = []
for i in range(7):
    o = 302 + i * 70
    pins += [f'<rect x="{o-14}" y="220" width="28" height="70" rx="6"/>',
             f'<rect x="{o-14}" y="734" width="28" height="70" rx="6"/>',
             f'<rect x="220" y="{o-14}" width="70" height="28" rx="6"/>',
             f'<rect x="734" y="{o-14}" width="70" height="28" rx="6"/>']
traces = []
for i in range(7):
    o = 302 + i * 70
    traces += [f'M {o} 220 V {120 + (i % 3) * 30}', f'M {o} 804 V {904 - (i % 3) * 30}',
               f'M 220 {o} H {120 + (i % 3) * 30}', f'M 804 {o} H {904 - (i % 3) * 30}']
ICONS["aok-silicon"] = ("Silicon", svg(
    f"""
<rect width="1024" height="1024" fill="#073b2c"/>
<path d="{' '.join(traces)}" stroke="#d4a72c" stroke-width="10" stroke-opacity="0.55" fill="none" stroke-linecap="round"/>
<g fill="#e3b341">{''.join(pins)}</g>
<rect x="270" y="270" width="484" height="484" rx="36" fill="#111827"/>
<rect x="300" y="300" width="424" height="424" rx="20" fill="url(#die)"/>
<circle cx="340" cy="340" r="14" fill="#374151"/>
<text x="512" y="560" text-anchor="middle" font-family="{SANS}" font-weight="900" font-size="150"
 fill="#e5e7eb" letter-spacing="2">AOK</text>
<text x="512" y="640" text-anchor="middle" font-family="{MONO}" font-weight="600" font-size="40" fill="#9ca3af">ARM · X86 · RV</text>
""",
    '<linearGradient id="die" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#1f2937"/>'
    '<stop offset="1" stop-color="#0b0f17"/></linearGradient>',
))

# 10. Sunset: striped retro sun in the AOK warm palette.
bands = "".join(
    f'<rect x="0" y="{y}" width="1024" height="{h}" fill="#082b33"/>'
    for y, h in [(500, 10), (540, 16), (586, 22), (638, 28)]
)
ICONS["aok-sunset"] = ("Sunset", svg(
    f"""
<rect width="1024" height="1024" fill="#082b33"/>
<g clip-path="url(#half)"><circle cx="512" cy="680" r="330" fill="url(#sun)"/>{bands}</g>
<rect x="80" y="680" width="864" height="8" rx="4" fill="#e86a2b"/>
<text x="512" y="900" text-anchor="middle" font-family="{SANS}" font-weight="900" font-size="190"
 fill="#f3efe6" letter-spacing="30">AOK</text>
""",
    '<linearGradient id="sun" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#d9a21b"/>'
    '<stop offset="0.5" stop-color="#e86a2b"/><stop offset="1" stop-color="#e8423f"/></linearGradient>'
    '<clipPath id="half"><rect x="0" y="0" width="1024" height="680"/></clipPath>',
))

# 11. A-OK: the O is a green check badge.
ICONS["aok-check"] = ("A-OK", svg(
    f"""
<rect width="1024" height="1024" fill="url(#g)"/>
<text x="230" y="640" text-anchor="middle" font-family="{SANS}" font-weight="900" font-size="340" fill="#0f172a">A</text>
<circle cx="512" cy="522" r="160" fill="#16a34a"/>
<path d="M 435 525 L 492 582 L 598 466" fill="none" stroke="#fff" stroke-width="44" stroke-linecap="round" stroke-linejoin="round"/>
<text x="800" y="640" text-anchor="middle" font-family="{SANS}" font-weight="900" font-size="340" fill="#0f172a">K</text>
""",
    '<linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/>'
    '<stop offset="1" stop-color="#dcfce7"/></linearGradient>',
))

# 12. Mono: plain black and white, block cursor.
ICONS["aok-mono"] = ("Mono", svg(
    f"""
<rect width="1024" height="1024" fill="#000"/>
<text x="470" y="620" text-anchor="middle" font-family="{SANS}" font-weight="900" font-size="290"
 fill="#fff" letter-spacing="-6">AOK</text>
<rect x="822" y="584" width="110" height="36" fill="#fff"/>
""",
))

for name, (desc, data) in ICONS.items():
    (OUT / f"{name}.svg").write_text(data, encoding="utf-8")
    print(name, desc)
