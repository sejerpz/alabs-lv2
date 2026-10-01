#!/usr/bin/env python3
"""Generates the ExpSense wiring diagrams (SVG) for the manual."""
ORANGE = "#e8771e"; BLUE = "#1f78c1"; GREY = "#6b7280"; INK = "#1f2328"; PANEL = "#2b2f36"

def defs():
    m = lambda i, c: (f'<marker id="{i}" viewBox="0 0 10 10" refX="10" refY="5" markerUnits="userSpaceOnUse" '
                      f'markerWidth="14" markerHeight="14" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="{c}"/></marker>')
    return f'''<defs>{m("aO", ORANGE)}{m("aB", BLUE)}{m("aG", GREY)}</defs>
<style>
  text {{ font-family: "Helvetica Neue", Helvetica, Arial, sans-serif; fill: {INK}; }}
  .title {{ font-size: 20px; font-weight: 700; }}
  .sub {{ font-size: 13px; fill: #4b5563; }}
  .lbl {{ font-size: 13px; font-weight: 600; }}
  .small {{ font-size: 11.5px; fill: #4b5563; }}
  .cablename {{ font-size: 15px; font-weight: 800; }}
  .jack {{ font-size: 12px; font-weight: 700; fill: #f3f4f6; letter-spacing: 0.5px; }}
  .dev {{ font-size: 13px; font-weight: 700; }}
  .cable {{ fill: none; stroke-width: 4; stroke-linecap: round; stroke-linejoin: round; }}
</style>'''

def box(x, y, w, h, label, sub=None):
    s = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="#f9fafb" stroke="{INK}" stroke-width="1.5"/>'
    s += f'<text class="lbl" x="{x + w / 2}" y="{y + h / 2 + (0 if sub else 5)}" text-anchor="middle">{label}</text>'
    if sub:
        s += f'<text class="small" x="{x + w / 2}" y="{y + h / 2 + 17}" text-anchor="middle">{sub}</text>'
    return s

PX, PY, PW, PH = 250, 180, 410, 100           # Dwarf rear panel
JY = PY + PH / 2                              # jack row
J = {'IN 1': 300, 'IN 2': 390, 'OUT 1': 510, 'OUT 2': 600}
UP = {'IN 1', 'OUT 1'}                        # plugs whose cable leaves upwards
PLUG_TOP = JY - 13 - 26
PLUG_BOT = JY + 13 + 26

def dwarf():
    s = f'<rect x="{PX}" y="{PY}" width="{PW}" height="{PH}" rx="10" fill="{PANEL}" stroke="#111" stroke-width="1.5"/>'
    for name, cx in J.items():
        s += (f'<circle cx="{cx}" cy="{JY}" r="13" fill="#111" stroke="#9ca3af" stroke-width="2"/>'
              f'<circle cx="{cx}" cy="{JY}" r="5" fill="#374151"/>')
        ly = JY + 33 if name in UP else JY - 22
        s += f'<text class="jack" x="{cx}" y="{ly}" text-anchor="middle">{name}</text>'
        py = JY - 13 - 26 if name in UP else JY + 13
        s += f'<rect x="{cx - 7}" y="{py}" width="14" height="26" rx="3" fill="#d1d5db" stroke="#4b5563" stroke-width="1.2"/>'
    s += (f'<path class="cable" d="M190,123 H300 V{PLUG_TOP}" stroke="{GREY}" marker-end="url(#aG)"/>'
          f'<path class="cable" d="M510,{PLUG_TOP} V123 H702" stroke="{GREY}" marker-end="url(#aG)"/>')
    s += (f'<text class="dev" x="{PX + PW + 14}" y="{PY + 44}">MOD Dwarf</text>'
          f'<text class="small" x="{PX + PW + 14}" y="{PY + 62}">rear panel</text>')
    return s

def pedal(cx, top):
    return (f'<rect x="{cx - 95}" y="{top + 42}" width="190" height="22" rx="6" fill="#4b5563" stroke="{INK}" stroke-width="1.5"/>'
            f'<path d="M{cx - 88},{top + 42} L{cx + 80},{top + 6} L{cx + 90},{top + 14} L{cx - 80},{top + 44} z" fill="#9ca3af" stroke="{INK}" stroke-width="1.5"/>'
            f'<circle cx="{cx - 70}" cy="{top + 44}" r="5" fill="{INK}"/>'
            f'<text class="lbl" x="{cx}" y="{top + 90}" text-anchor="middle">Passive expression pedal</text>'
            f'<text class="small" x="{cx}" y="{top + 107}" text-anchor="middle">TRS jack · Tip = wiper · Ring = pot end · Sleeve = ground</text>')

def pedal_y(yj):
    """Y cable TRS <-> 2 x TS whose TRS end goes to the pedal."""
    return [f'<circle cx="500" cy="{yj}" r="11" fill="{INK}"/>',
            f'<path class="cable" d="M500,{yj + 11} V{yj + 40}" stroke="{INK}"/>',
            f'<rect x="491" y="{yj + 40}" width="18" height="24" rx="3" fill="#d1d5db" stroke="#4b5563" stroke-width="1.2"/>',
            f'<text class="cablename" x="522" y="{yj + 24}">Y cable · TRS ⟷ 2 × TS</text>',
            f'<text class="small" x="522" y="{yj + 41}">insert cable, or TRS female adapter + stereo cable</text>',
            pedal(500, yj + 56)]

def legs(y_label, ring_sub):
    return (f'<text class="lbl" x="612" y="{y_label}" style="fill:{ORANGE}">Ring · R · Return</text>'
            f'<text class="small" x="612" y="{y_label + 17}">{ring_sub}</text>'
            f'<text class="lbl" x="378" y="{y_label}" text-anchor="end" style="fill:{BLUE}">Tip · L · Send</text>'
            f'<text class="small" x="378" y="{y_label + 17}" text-anchor="end">to IN 2</text>')

def legend(x, y):
    row = lambda dy, c, m, t: (f'<line x1="0" y1="{dy}" x2="30" y2="{dy}" stroke="{c}" stroke-width="4" marker-end="url(#{m})"/>'
                               f'<text class="small" x="48" y="{dy + 4}">{t}</text>')
    return (f'<g transform="translate({x},{y})">' + row(0, ORANGE, 'aO', 'Out 2 → pedal (music + pilot tone)')
            + row(22, BLUE, 'aB', 'Pedal wiper → In 2 (position)') + row(44, GREY, 'aG', 'Instrument / amplifier') + '</g>')

def svg(w, h, body):
    return '\n'.join([f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">', defs(),
                      f'<rect width="{w}" height="{h}" fill="#ffffff"/>'] + body + ['</svg>'])

def diagram_a():
    yj = 420
    body = [f'<text class="title" x="40" y="44">A · One Y cable (mono)</text>',
            f'<text class="sub" x="40" y="66">The simplest setup: Out 1 is the main (mono) output, Out 2 and In 2 are used by the pedal.</text>',
            box(60, 96, 130, 54, 'Guitar'), box(710, 96, 130, 54, 'Amplifier', 'mono'), dwarf(),
            f'<path class="cable" d="M600,{PLUG_BOT} V330 Q600,{yj} 520,{yj}" stroke="{ORANGE}"/>',
            f'<path d="M600,{PLUG_BOT + 30} v10" stroke="{ORANGE}" stroke-width="4" marker-end="url(#aO)"/>',
            f'<path class="cable" d="M480,{yj} Q390,{yj} 390,330 V{PLUG_BOT + 1}" stroke="{BLUE}" marker-end="url(#aB)"/>',
            legs(328, 'to OUT 2'), *pedal_y(yj), legend(40, 530)]
    return svg(900, 600, body)

def diagram_b():
    ys, yj = 330, 470
    body = [f'<text class="title" x="40" y="44">B · Two Y cables (stereo)</text>',
            f'<text class="sub" x="40" y="66">A Y splitter keeps Out 2 available for the right amp, while also feeding the pedal.</text>',
            box(60, 96, 130, 54, 'Guitar'), box(710, 96, 130, 54, 'Amplifier L', 'or mixer L'), dwarf(),
            f'<path class="cable" d="M600,{PLUG_BOT} V{ys - 14}" stroke="{ORANGE}"/>',
            f'<rect x="580" y="{ys - 14}" width="40" height="28" rx="6" fill="#f3f4f6" stroke="{INK}" stroke-width="1.5"/>',
            f'<text class="cablename" x="632" y="{ys - 22}">Y splitter · TS → 2 × TS</text>',
            f'<text class="small" x="632" y="{ys - 6}">mono, one jack to two</text>',
            f'<path class="cable" d="M620,{ys + 6} H700 Q775,{ys + 6} 775,{ys + 56} V{ys + 76}" stroke="{GREY}" marker-end="url(#aG)"/>',
            box(710, ys + 84, 130, 54, 'Amplifier R', 'or mixer R'),
            f'<path class="cable" d="M600,{ys + 14} V{yj - 70} Q600,{yj} 520,{yj}" stroke="{ORANGE}"/>',
            f'<path d="M600,{ys + 34} v10" stroke="{ORANGE}" stroke-width="4" marker-end="url(#aO)"/>',
            f'<path class="cable" d="M480,{yj} Q390,{yj} 390,{yj - 70} V{PLUG_BOT + 1}" stroke="{BLUE}" marker-end="url(#aB)"/>',
            legs(ys + 72, 'from the Y splitter'), *pedal_y(yj), legend(40, 590)]
    return svg(900, 660, body)

if __name__ == '__main__':
    open('wiring-a-single-cable.svg', 'w').write(diagram_a())
    open('wiring-b-two-y-cables.svg', 'w').write(diagram_b())
    print('written')
