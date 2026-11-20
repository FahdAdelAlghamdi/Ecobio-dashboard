# -*- coding: utf-8 -*-
"""Builds ../diagram.json for Wokwi (EcoBio-Matrix, phase 2) with tidy, hand-routed wires.

    python make_diagram.py      -> writes ../diagram.json and prints a wiring check

Pin numbers must match the PIN_* constants in ../sketch.ino.
Every wire is drawn as straight horizontal/vertical segments. The checker at the end makes sure
that wires of different nets never run on top of each other, never touch, and never pass over a part.
Where two different wires cross at a right angle they are NOT connected (Wokwi only connects at pins).
"""
import json, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(os.path.dirname(HERE), "diagram.json")
MM = 96 / 25.4  # Wokwi units per mm

# ---------- part geometry in Wokwi units (unrotated), measured in the Wokwi editor
SIZE = {
    "board-esp32-devkit-c-v4": (27.9 * MM, 56.628 * MM),
    "wokwi-slide-potentiometer": (208, 110),
    "wokwi-ds18b20": (32.4, 50.6),
    "wokwi-hc-sr04": (170, 94.5),
    "wokwi-relay-module": (128, 58.2),
    "wokwi-led": (40, 50),
    "wokwi-resistor": (58.8, 11.3),
    "wokwi-pushbutton": (67.3, 60.4),
}
PINS = {
    "wokwi-slide-potentiometer": {"VCC": (1, 43), "SIG": (1, 63), "GND": (207, 43)},
    "wokwi-ds18b20": {"GND": (5.52, 49.13), "DQ": (15.12, 49.13), "VCC": (24.72, 49.13)},
    "wokwi-hc-sr04": {"VCC": (71.3, 94.5), "TRIG": (81.3, 94.5), "ECHO": (91.3, 94.5), "GND": (101.3, 94.5)},
    "wokwi-relay-module": {"VCC": (0, 19), "GND": (0, 29), "IN": (0, 38.4),
                           "NC": (114, 20.2), "COM": (114, 30), "NO": (114, 40)},
    "wokwi-led": {"C": (15, 42), "A": (25, 42)},
    "wokwi-resistor": {"1": (0, 5.65), "2": (58.8, 5.65)},
    "wokwi-pushbutton": {"1.l": (0, 13), "2.l": (0, 32), "1.r": (67, 13), "2.r": (67, 32)},
}
_L = ["3V3", "EN", "VP", "VN", "34", "35", "32", "33", "25", "26", "27", "14", "12", "GND.1",
      "13", "D2", "D3", "CMD", "5V"]
_R = ["GND.2", "23", "22", "TX", "RX", "21", "GND.3", "19", "18", "5", "17", "16", "4", "0",
      "2", "15", "D1", "D0", "CLK"]
PINS["board-esp32-devkit-c-v4"] = {}
for i, n in enumerate(_L):
    PINS["board-esp32-devkit-c-v4"][n] = (1.22 * MM, (7.62 + 2.54 * i) * MM)
for i, n in enumerate(_R):
    PINS["board-esp32-devkit-c-v4"][n] = (26.66 * MM, (7.62 + 2.54 * i) * MM)

parts, conns, texts, wires = [], [], [], []
P = {}  # id -> part dict


def part(t, pid, left, top, attrs=None, rotate=0):
    p = {"type": t, "id": pid, "top": round(top, 2), "left": round(left, 2), "attrs": attrs or {}}
    if rotate:
        p["rotate"] = rotate
    parts.append(p)
    P[pid] = p


def text(pid, left, top, s):
    part("wokwi-text", pid, left, top, {"text": s})
    texts.append((pid, left, top, s))


def _rot(t, rot, x, y):
    w, h = SIZE[t]
    cx, cy = w / 2, h / 2
    dx, dy = x - cx, y - cy
    if rot == 90:
        dx, dy = -dy, dx
    elif rot == 180:
        dx, dy = -dx, -dy
    elif rot == 270:
        dx, dy = dy, -dx
    return cx + dx, cy + dy


def pin(ref):
    pid, name = ref.split(":", 1)
    p = P[pid]
    x, y = PINS[p["type"]][name]
    x, y = _rot(p["type"], p.get("rotate", 0), x, y)
    return (p["left"] + x, p["top"] + y)


def bbox(pid):
    p = P[pid]
    w, h = SIZE[p["type"]]
    x0, y0 = _rot(p["type"], p.get("rotate", 0), 0, 0)
    x1, y1 = _rot(p["type"], p.get("rotate", 0), w, h)
    return (p["left"] + min(x0, x1), p["top"] + min(y0, y1), p["left"] + max(x0, x1), p["top"] + max(y0, y1))


def _fmt(v):
    s = ("%.2f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def wire(a, b, color, via=()):
    """Wire from pin a to pin b through the given absolute corner points (horizontal/vertical only)."""
    pts = [pin(a)] + [tuple(v) for v in via] + [pin(b)]
    # drop repeated points and merge straight runs
    clean = [pts[0]]
    for q in pts[1:]:
        if abs(q[0] - clean[-1][0]) < 1e-6 and abs(q[1] - clean[-1][1]) < 1e-6:
            continue
        clean.append(q)
    merged = [clean[0]]
    for i in range(1, len(clean)):
        if len(merged) >= 2:
            (x0, y0), (x1, y1), (x2, y2) = merged[-2], merged[-1], clean[i]
            if (abs(x0 - x1) < 0.01 and abs(x1 - x2) < 0.01) or (abs(y0 - y1) < 0.01 and abs(y1 - y2) < 0.01):
                merged[-1] = clean[i]
                continue
        merged.append(clean[i])
    for (x0, y0), (x1, y1) in zip(merged, merged[1:]):
        if abs(x0 - x1) > 0.05 and abs(y0 - y1) > 0.05:
            sys.exit(f"diagonal segment in wire {a} -> {b}: {(x0, y0)} -> {(x1, y1)}")
    # Wokwi stretches the last instruction to line up with the target pin and then draws the final
    # leg itself, so we emit every segment except the last one.
    ins = []
    for (x0, y0), (x1, y1) in zip(merged[:-2], merged[1:-1]):
        ins.append(("h" + _fmt(x1 - x0)) if abs(y1 - y0) < 0.05 else ("v" + _fmt(y1 - y0)))
    conns.append([a, b, color, ins])
    wires.append((a, b, merged))


# =====================================================================================
# Layout (Wokwi units). ESP32 at the origin. Sensors on the left, outputs on the right.
# =====================================================================================
part("board-esp32-devkit-c-v4", "esp", 0, 0)
conns.append(["esp:TX", "$serialMonitor:RX", "", []])
conns.append(["esp:RX", "$serialMonitor:TX", "", []])
text("title", -505, -455, "EcoBio-Matrix · ESP32 · Wokwi")

E = lambda n: pin("esp:" + n)

# ---------------- left: six slide pots in one column = analog sensors (ADC1 pins only)
#   start values (normal water): pH 7.5, ORP1 +181 mV, DO 3.79 mg/L, turbidity 2 NTU, UV 85 %, ORP2 700 mV
#   ORP1 and DO sit at what the dashboard's AI model calls normal (phase 4: ORP 180 mV, DO 3.8 mg/L with the air pump on).
PX, PT0, PSTEP = -480, -330, 150
X_VCC, X_JOG, X_GND = -505, -492, -250          # red rail, signal jog, black rail
POTS = [  # id, ESP pin, start value, label, fan-out x
    ("pot_ph",   "VP", 548, "pH (الموازنة) · GPIO36",         -122),
    ("pot_orp1", "VN", 604, "ORP1 (الموازنة) · GPIO39",       -134),
    ("pot_do",   "34", 194, "DO الأكسجين بالمفاعل · GPIO34",  -108),
    ("pot_turb", "35", 102, "العكارة قبل الأشعة · GPIO35", -96),
    ("pot_uv",   "32", 870, "UV شدة الأشعة · GPIO32",          -84),
    ("pot_orp2", "33", 512, "ORP2 (بعد الكلور) · GPIO33",      -72),
]
for i, (pid, pn, val, label, xf) in enumerate(POTS):
    top = PT0 + i * PSTEP
    text("t_" + pid, PX + 2, top - 24, label)
    part("wokwi-slide-potentiometer", pid, PX, top, {"travelLength": "30", "value": str(val)})
for i, (pid, pn, val, label, xf) in enumerate(POTS):
    top = PT0 + i * PSTEP
    lane = top + 118
    wire(f"{pid}:SIG", "esp:" + pn, "green",
         [(X_JOG, top + 63), (X_JOG, lane), (xf, lane), (xf, E(pn)[1])])
    if i < len(POTS) - 1:
        nxt = POTS[i + 1][0]
        ntop = top + PSTEP
        wire(f"{pid}:VCC", f"{nxt}:VCC", "red", [(X_VCC, top + 43), (X_VCC, ntop + 43)])
        wire(f"{pid}:GND", f"{nxt}:GND", "black", [(X_GND, top + 43), (X_GND, ntop + 43)])

# 3V3 to the red rail, over the top of the column
wire("pot_ph:VCC", "esp:3V3", "red", [(X_VCC, PT0 + 43), (X_VCC, -375), (-30, -375), (-30, E("3V3")[1])])
# black rail to ESP GND.2, over the top of everything
wire("pot_ph:GND", "esp:GND.2", "black", [(X_GND, PT0 + 43), (X_GND, -395), (120, -395), (120, E("GND.2")[1])])

# ---------------- bottom left: DS18B20 (turned over so its pins face the wires) + 4.7k pull-up, HC-SR04
# Only the DS18B20 is rotated: Wokwi draws wires to rotated LEDs/resistors in the wrong place on first load.
part("wokwi-ds18b20", "temp", -470, 645, {"temperature": "25"}, rotate=180)
text("t_temp", -480, 703, "DS18B20 الحرارة · GPIO25")
part("wokwi-resistor", "r_pull", -420, 594.35, {"value": "4700"})
last = POTS[-1][0]
rp1, rp2 = pin("r_pull:1"), pin("r_pull:2")
tv, tq, tg = pin("temp:VCC"), pin("temp:DQ"), pin("temp:GND")
wire(f"{last}:VCC", "r_pull:1", "red", [(X_VCC, PT0 + 5 * PSTEP + 43), (X_VCC, rp1[1])])
wire("temp:VCC", "r_pull:1", "red", [(tv[0], rp1[1])])
wire("esp:25", "temp:DQ", "orange", [(-60, E("25")[1]), (-60, 620), (tq[0], 620)])
wire("r_pull:2", "temp:DQ", "orange", [(-350, rp2[1]), (-350, 620), (tq[0], 620)])
wire(f"{last}:GND", "temp:GND", "black", [(X_GND, PT0 + 5 * PSTEP + 43), (X_GND, 635), (tg[0], 635)])

part("wokwi-hc-sr04", "level", -360, 690, {"distance": "80"})
text("t_level", -360, 640, "HC-SR04 مستوى التخزين\nTRIG GPIO26 · ECHO GPIO27")
hv, ht, he, hg = pin("level:VCC"), pin("level:TRIG"), pin("level:ECHO"), pin("level:GND")
wire("level:GND", "temp:GND", "black", [(hg[0], 792), (-170, 792), (-170, 635), (tg[0], 635)])
wire("esp:26", "level:TRIG", "blue", [(-48, E("26")[1]), (-48, 800), (ht[0], 800)])
wire("esp:27", "level:ECHO", "purple", [(-36, E("27")[1]), (-36, 812), (he[0], 812)])
wire("esp:5V", "level:VCC", "red", [(-12, E("5V")[1]), (-12, 824), (hv[0], 824)])

# ---------------- right: four relays in one column, each with its own lamp pair
#   green lamp = NO side (normal path / running), red lamp = NC side (sewer / stopped)
RX = 420
RY = [-117.4, 57.6, 232.6, 407.6]      # relay 2 IN lines up exactly with GPIO19
X_5V = 800                              # 5 V rail on the far right (feeds every COM)
RELAYS = [  # id, ESP pin, label, fan-out x
    ("rl_v1",  "21", "GPIO21 · صمام 1 (سليم؟)", 156),
    ("rl_v2",  "19", "GPIO19 · صمام 2 (مطابق؟)", None),
    ("rl_air", "18", "GPIO18 · مضخة الهواء", 182),
    ("rl_cl",  "17", "GPIO17 · مضخة الكلور", 170),
]
for i, (pid, pn, label, xf) in enumerate(RELAYS):
    text("t_" + pid, RX, RY[i] - 78, label)
    part("wokwi-relay-module", pid, RX, RY[i])
for i, (pid, pn, label, xf) in enumerate(RELAYS):
    r = RY[i]
    # control line from the ESP32
    iny = r + 38.4
    if xf is None:
        wire("esp:" + pn, f"{pid}:IN", "blue")
    else:
        wire("esp:" + pn, f"{pid}:IN", "blue", [(xf, E(pn)[1]), (xf, iny)])
    # resistors + lamps
    part("wokwi-resistor", f"rr_{pid}", 576, r + 6.35, {"value": "220"})
    part("wokwi-led", f"lr_{pid}", 650, r - 40, {"color": "red"})
    part("wokwi-resistor", f"rg_{pid}", 576, r + 94.35, {"value": "220"})
    part("wokwi-led", f"lg_{pid}", 650, r + 40, {"color": "limegreen"})
    wire(f"{pid}:NC", f"rr_{pid}:1", "darkorange", [(556, r + 20.2), (556, r + 12)])
    wire(f"rr_{pid}:2", f"lr_{pid}:A", "darkorange", [(pin(f"lr_{pid}:A")[0], r + 12)])
    wire(f"{pid}:NO", f"rg_{pid}:1", "limegreen", [(556, r + 40), (556, r + 100)])
    wire(f"rg_{pid}:2", f"lg_{pid}:A", "limegreen", [(pin(f"lg_{pid}:A")[0], r + 100)])
    # relay power: VCC from its own COM (5 V), GND from the line above the relay
    wire(f"{pid}:COM", f"{pid}:VCC", "red", [(564, r + 30), (564, r - 14), (414, r - 14), (414, r + 19)])
    lrC = pin(f"lr_{pid}:C")
    wire(f"lr_{pid}:C", f"{pid}:GND", "black",
         [(lrC[0], r + 6), (644, r + 6), (644, r - 50), (408, r - 50), (408, r + 29)])
    lgC = pin(f"lg_{pid}:C")
    if i < len(RELAYS) - 1:     # green lamp ground joins the next relay's ground line
        nr = RY[i + 1]
        wire(f"lg_{pid}:C", f"{RELAYS[i + 1][0]}:GND", "black",
             [(lgC[0], r + 93), (644, r + 93), (644, nr - 50), (408, nr - 50), (408, nr + 29)])
        wire(f"{pid}:COM", f"{RELAYS[i + 1][0]}:COM", "red",
             [(X_5V, r + 30), (X_5V, nr + 30)])
        # ground spine on the left of the relays (joins the relays' grounds together)
        wire(f"{pid}:GND", f"{RELAYS[i + 1][0]}:GND", "black", [(408, r + 29), (408, nr + 29)])
    else:
        wire(f"lg_{pid}:C", f"{pid}:GND", "black",
             [(lgC[0], r + 93), (644, r + 93), (644, r + 125), (408, r + 125), (408, r + 29)])
# 5 V from the ESP32 to the 5 V rail (under everything)
wire("esp:5V", "rl_cl:COM", "red", [(-12, E("5V")[1]), (-12, 850), (X_5V, 850), (X_5V, RY[3] + 30)])

# ---------------- buttons (keyboard: F = sensor fault, N = internet cut), top right
BY = -330
part("wokwi-pushbutton", "btn_fault", 200, BY, {"color": "red", "label": "عطل حساس (F) GPIO23", "key": "f", "bounce": "0"})
part("wokwi-pushbutton", "btn_net", 300, BY, {"color": "blue", "label": "قطع الإنترنت (N) GPIO22", "key": "n", "bounce": "0"})
wire("esp:23", "btn_fault:1.l", "gold", [(132, E("23")[1]), (132, BY + 13)])
wire("esp:22", "btn_net:1.l", "gold", [(144, E("22")[1]), (144, BY + 95), (288, BY + 95), (288, BY + 13)])
wire("esp:GND.2", "btn_fault:2.r", "black", [(120, E("GND.2")[1]), (120, BY - 15), (280, BY - 15), (280, BY + 32)])
wire("btn_fault:2.r", "btn_net:2.r", "black", [(280, BY + 32), (280, BY - 15), (380, BY - 15), (380, BY + 32)])
wire("btn_net:2.r", "rl_v1:GND", "black", [(380, BY + 32), (380, RY[0] - 50), (408, RY[0] - 50), (408, RY[0] + 29)])

diagram = {"version": 1, "author": "EcoBio-Matrix team", "editor": "wokwi",
           "parts": parts, "connections": conns, "dependencies": {}}
with open(OUT, "w", encoding="utf-8") as f:
    json.dump(diagram, f, ensure_ascii=False, indent=2)
print("wrote", OUT, len(parts), "parts,", len(conns), "connections")


# =====================================================================================
# Wiring check
# =====================================================================================
parent = {}


def find(a):
    parent.setdefault(a, a)
    while parent[a] != a:
        parent[a] = parent[parent[a]]
        a = parent[a]
    return a


def union(a, b):
    parent[find(a)] = find(b)


for a, b, _ in wires:
    union(a, b)
union("esp:GND.1", "esp:GND.2"); union("esp:GND.2", "esp:GND.3")
segs = []  # (net, wire index, x0, y0, x1, y1)
for k, (a, b, pts) in enumerate(wires):
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        segs.append((find(a), k, x0, y0, x1, y1))

problems, crossings = [], []
EPS = 0.6


def horiz(s):
    return abs(s[3] - s[5]) < 0.05


for i in range(len(segs)):
    for j in range(i + 1, len(segs)):
        s, t = segs[i], segs[j]
        if s[0] == t[0]:
            continue
        if horiz(s) == horiz(t):  # parallel: must not overlap
            if horiz(s) and abs(s[3] - t[3]) < 3:
                lo, hi = max(min(s[2], s[4]), min(t[2], t[4])), min(max(s[2], s[4]), max(t[2], t[4]))
                if hi - lo > -EPS:
                    problems.append(("too close / overlap", wires[s[1]][:2], wires[t[1]][:2]))
            if not horiz(s) and abs(s[2] - t[2]) < 3:
                lo, hi = max(min(s[3], s[5]), min(t[3], t[5])), min(max(s[3], s[5]), max(t[3], t[5]))
                if hi - lo > -EPS:
                    problems.append(("too close / overlap", wires[s[1]][:2], wires[t[1]][:2]))
            continue
        h, v = (s, t) if horiz(s) else (t, s)
        hx0, hx1 = sorted((h[2], h[4])); vy0, vy1 = sorted((v[3], v[5]))
        x, y = v[2], h[3]
        if hx0 - EPS <= x <= hx1 + EPS and vy0 - EPS <= y <= vy1 + EPS:
            inner = hx0 + EPS < x < hx1 - EPS and vy0 + EPS < y < vy1 - EPS
            if inner:
                crossings.append((wires[h[1]][:2], wires[v[1]][:2], (round(x), round(y))))
            else:
                problems.append(("touch (looks connected)", wires[h[1]][:2], wires[v[1]][:2], (round(x), round(y))))

# wires must not run over parts (except right next to their own pins) or over labels
boxes = []
for p in parts:
    if p["type"] in SIZE:
        boxes.append((p["id"], bbox(p["id"])))
for pid, x, y, s in texts:
    lines = s.splitlines()
    boxes.append((pid, (x, y + 2, x + 7.0 * max(len(l) for l in lines), y + 2 + 18 * len(lines))))
for k, (a, b, pts) in enumerate(wires):
    ends = {}
    for ref in (a, b):
        ends.setdefault(ref.split(":")[0], []).append(pin(ref))
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        n = int(max(abs(x1 - x0), abs(y1 - y0)) // 2) + 1
        for t in range(n + 1):
            x, y = x0 + (x1 - x0) * t / n, y0 + (y1 - y0) * t / n
            for pid, (bx0, by0, bx1, by1) in boxes:
                if pid == "esp" and pid in ends:
                    if bx0 + 8 < x < bx1 - 8 and by0 < y < by1:
                        problems.append(("over ESP32", (a, b), (round(x), round(y))))
                    continue
                if bx0 + 1 < x < bx1 - 1 and by0 + 1 < y < by1 - 1:
                    if pid in ends and min(abs(x - q[0]) + abs(y - q[1]) for q in ends[pid]) < 18:
                        continue
                    problems.append(("over part/label " + pid, (a, b), (round(x), round(y))))
                    break

seen = set()
for pr in problems:
    key = str(pr[:3])
    if key in seen:
        continue
    seen.add(key)
    print("PROBLEM:", pr)
print(f"{len(crossings)} right-angle crossings (not connected):")
for c in crossings:
    print("   ", c)
