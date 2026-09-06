#!/usr/bin/env python3
"""Generate the mode diagrams for implementation-plan.md.

Every dimension comes from the constants block below, which mirrors the plan.
Regenerate with:  python3 gen.py
"""
import math, os

# ---- constants (mirror implementation-plan.md §2.1, §4.2) -------------------
T_SYM   = 0.256                      # ms
N_PRE   = 8
T_PRE   = (N_PRE + 4.25) * T_SYM     # 3.136 ms, air start -> T0 (SFD end)
T_HDR   = 8 * T_SYM                  # 2.048 ms, T0 -> ValidHeader
D_TX    = 0.220                      # ms, RegOpMode=TX -> air start (UNMEASURED)
ROUND   = 1500.0
SLOTS   = 32
PITCH   = ROUND / SLOTS              # 46.875 ms
WIN     = 115 * T_SYM                # 29.44 ms
T_DET   = 5 * T_SYM                  # 1.28 ms (UNMEASURED)
GUARD   = (WIN - T_DET) / 2          # 14.08 ms
ARM     = T_PRE + GUARD              # 17.216 ms before T0
FIRE    = T_PRE + D_TX               # 3.356 ms before T0
RXI_A   = 500.0                      # Mode A window interval
COPY_A  = 88.0                       # Mode A copy spacing
NCOPY   = 17

def nsym(pl): return 8 + max(math.ceil((8*pl - 4*7 + 28 + 16)/28)*8, 0)
def toa(pl):  return T_PRE + nsym(pl)*T_SYM
ACK, BCN, CMD, SCH = toa(25), toa(45), toa(60), toa(152)

# ---- tiny svg helper --------------------------------------------------------
INK, MUTE, GRID = "#1b1f24", "#57606a", "#d8dee4"
RX, TX, BCNC, SLEEP, WARN = "#0969da", "#bc4c00", "#8250df", "#59636e", "#cf222e"
FILL_RX, FILL_TX, FILL_BCN = "#ddf4ff", "#fff1e5", "#fbefff"

class S:
    def __init__(s, w, h, title):
        s.w, s.h, s.o = w, h, []
        s.o.append(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" '
                   f'width="{w}" height="{h}" font-family="ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif">')
        s.o.append(f'<rect width="{w}" height="{h}" fill="#ffffff"/>')
        s.txt(16, 26, title, 15, INK, w=600)
    def txt(s, x, y, t, sz=11, c=INK, anchor="start", w=400, mono=False):
        fam = ' font-family="ui-monospace,SFMono-Regular,Menlo,monospace"' if mono else ''
        t = t.replace("&","&amp;").replace("<","&lt;").replace(">","&gt;")
        s.o.append(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{sz}" fill="{c}" '
                   f'text-anchor="{anchor}" font-weight="{w}"{fam}>{t}</text>')
    def rect(s, x, y, w, h, fill, stroke=None, rx=2, op=1.0):
        st = f' stroke="{stroke}" stroke-width="1"' if stroke else ''
        s.o.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{max(w,0.6):.1f}" height="{h:.1f}" '
                   f'fill="{fill}" opacity="{op}" rx="{rx}"{st}/>')
    def line(s, x1, y1, x2, y2, c=GRID, w=1, dash=None):
        d = f' stroke-dasharray="{dash}"' if dash else ''
        s.o.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
                   f'stroke="{c}" stroke-width="{w}"{d}/>')
    def path(s, d, c, w=1.6, fill="none"):
        s.o.append(f'<path d="{d}" stroke="{c}" stroke-width="{w}" fill="{fill}" '
                   f'stroke-linejoin="round"/>')
    def arrow(s, x1, y, x2, c=MUTE, label=None, sz=10):
        s.line(x1, y, x2, y, c, 1)
        for x, dx in ((x1, 4), (x2, -4)):
            s.o.append(f'<path d="M{x:.1f},{y} l{dx},-3 l0,6 z" fill="{c}"/>')
        if label: s.txt((x1+x2)/2, y-5, label, sz, c, "middle", 500)
    def save(s, name):
        s.o.append("</svg>")
        open(os.path.join(os.path.dirname(os.path.abspath(__file__)), name), "w").write("\n".join(s.o))
        print(f"  {name}")

# ============================================================ 1. frame anatomy
def d1():
    W, H = 980, 300
    s = S(W, H, "1 — Frame anatomy: T0 = SFD end is the reference both ends name")
    x0, span = 90.0, 820.0
    total = T_PRE + T_HDR + 12.0                      # show preamble+header+a little payload
    px = lambda ms: x0 + ms / total * span
    y = 92
    # air trace
    s.rect(px(0), y, px(T_PRE)-px(0), 34, FILL_TX, TX)
    s.rect(px(T_PRE), y, px(T_PRE+T_HDR)-px(T_PRE), 34, "#ffe0c2", TX)
    s.rect(px(T_PRE+T_HDR), y, px(total)-px(T_PRE+T_HDR), 34, "#f6f8fa", MUTE)
    s.txt(px(T_PRE/2), y+21, "preamble + sync + SFD", 10.5, TX, "middle", 600)
    s.txt(px(T_PRE+T_HDR/2), y+21, "header", 10.5, TX, "middle", 600)
    s.txt(px(T_PRE+T_HDR+6), y+21, "payload …", 10.5, MUTE, "middle")
    s.txt(x0-10, y+21, "air", 11, INK, "end", 600)
    # T0 marker
    s.line(px(T_PRE), y-30, px(T_PRE), y+92, INK, 2)
    s.txt(px(T_PRE), y-36, "T0  (SFD end)", 12.5, INK, "middle", 700)
    # intervals
    s.arrow(px(0), y+52, px(T_PRE), TX, f"T_pre = {T_PRE*1000:.0f} µs")
    s.arrow(px(T_PRE), y+52, px(T_PRE+T_HDR), TX, f"T_hdr = {T_HDR*1000:.0f} µs")
    # events
    for xm, lab, c, anc, dx, ly in ((0, "air start", MUTE, "start", 6, 70),
                                    (-D_TX, "hub FIRE — RegOpMode = TX", TX, "start", 6, 88),
                                    (T_PRE+T_HDR, "ValidHeader — DIO3 NOT ROUTED", WARN, "middle", 0, 70)):
        X = px(xm)
        s.line(X, y-8, X, y+ly-12, c, 1, "3 3")
        s.txt(X+dx, y+ly, lab, 10, c, anc, 600)
    # node side
    yn = y + 118
    s.txt(x0-10, yn+14, "node", 11, INK, "end", 600)
    s.txt(px(T_PRE+T_HDR+6), yn+14, "RxDone  →  T0 = t_rxdone − n_sym(len)·T_sym", 11, RX, "middle", 600)
    s.line(px(total), yn-4, px(total), yn+20, RX, 1.6)
    s.txt(x0, yn+42, "hub:   T0 = t_fire + d_tx_ramp + T_pre        (d_tx_ramp ≈ 220 µs — UNMEASURED)", 10.5, MUTE, mono=True)
    s.txt(x0, yn+58, "node:  T0 = t_rxdone − n_sym(len)·T_sym       one line; never subtract T_hdr separately", 10.5, MUTE, mono=True)
    s.txt(x0, yn+80, f"T_sym {T_SYM*1000:.0f} µs   ·   ack 25 B {ACK:.1f} ms   ·   beacon 45 B {BCN:.1f} ms   ·   "
                     f"command 60 B {CMD:.1f} ms   ·   ScheduleConfig 152 B {SCH:.1f} ms", 10.5, MUTE)
    s.save("01-frame-anatomy.svg")

# ================================================================== 2. mode A
def d2():
    W, H = 980, 320
    s = S(W, H, "2 — Mode A (today): 17 copies sweep across a windowed receiver")
    x0, span = 92.0, 830.0
    px = lambda ms: x0 + ms / ROUND * span
    # hub copies
    y = 74
    s.txt(x0-10, y+16, "hub", 11, INK, "end", 600)
    for i in range(NCOPY):
        s.rect(px(i*COPY_A), y, px(CMD)-px(0), 24, FILL_TX, TX)
    s.txt(px(ROUND/2), y-10, f"{NCOPY} copies · {COPY_A:.0f} ms apart · each {CMD:.1f} ms on air", 10.5, TX, "middle", 600)
    # node 3 windows
    y2 = y + 62
    s.txt(x0-10, y2+16, "node", 11, INK, "end", 600)
    phase = 22.0
    for k in range(3):
        s.rect(px(phase + k*RXI_A), y2, px(WIN)-px(0), 24, FILL_RX, RX)
    s.txt(px(ROUND/2), y2+48, f"3 windows · {WIN:.2f} ms every {RXI_A:.0f} ms · duty 5.9 %", 10.5, RX, "middle", 600)
    # catches
    for k in range(3):
        ws, we = phase + k*RXI_A, phase + k*RXI_A + WIN
        for i in range(NCOPY):
            c = i*COPY_A
            if ws <= c < we:
                s.line(px(c)+3, y+24, px(c)+3, y2, "#1a7f37", 1.4, "2 2")
                s.o.append(f'<circle cx="{px(c)+3:.1f}" cy="{y2-3:.1f}" r="3" fill="#1a7f37"/>')
    # comparison box
    yb = y2 + 76
    s.rect(x0, yb, span, 96, "#f6f8fa", GRID, 4)
    rows = [("3 windows, 17 copies  (today)", "96.4 %", "#1a7f37"),
            ("1 window, 17 copies, unsynchronised", "32.9 %", WARN),
            ("1 window, 1 copy, synchronised  (Mode B)", "~100 %", "#1a7f37")]
    s.txt(x0+16, yb+22, "P( ≥1 copy heard per round )", 11, INK, w=600)
    for i,(lab,val,c) in enumerate(rows):
        s.txt(x0+16, yb+44+i*18, lab, 10.5, MUTE)
        s.txt(x0+400, yb+44+i*18, val, 11, c, w=700, mono=True)
    s.txt(x0+470, yb+44, "88 ms against 500 ms is incommensurate, so copies sweep", 10, MUTE)
    s.txt(x0+470, yb+60, "across the window. Treating the 3 windows as independent", 10, MUTE)
    s.txt(x0+470, yb+76, "gives 69.8 % — wrong by 27 points.", 10, MUTE)
    s.save("02-mode-a.svg")

# ============================================================ 3. mode B round
def d3():
    W, H = 980, 540
    s = S(W, H, "3 — Mode B round: 32 private windows of 46.875 ms in 1500 ms")
    x0, span = 96.0, 800.0
    px = lambda ms: x0 + ms / ROUND * span
    K = 7
    # ---- row 1 : the whole round -------------------------------------------
    y = 78
    s.txt(x0-10, y+15, "round", 11, INK, "end", 700)
    for k in range(SLOTS):
        s.rect(px(k*PITCH-ARM), y, px(WIN)-px(0), 22, FILL_RX if k != K else "#b6e3ff",
               RX if k != K else "#0550ae")
    s.txt(px(K*PITCH), y-9, f"node k={K}", 10, "#0550ae", "middle", 700)
    s.txt(x0, y+40, f"{SLOTS} windows · {WIN:.2f} ms each · pitch {PITCH:.3f} ms · "
                    f"{PITCH-WIN:.1f} ms clear between adjacent windows · duty 1.96 %", 10.5, MUTE)
    s.line(x0, y+56, x0+span, y+56, GRID, 1)

    # ---- row 2 : zoom -------------------------------------------------------
    zx0, zspan, ztot = 96.0, 800.0, 8*PITCH
    zp = lambda ms: zx0 + (ms+ARM)/ztot*zspan          # origin at the first ARM, so nothing clips
    s.txt(zx0, 158, f"zoom — slots k … k+7  ({ztot:.0f} ms of the round above)", 10.5, INK, w=700)
    yh, yw, ya = 194, 250, 308
    s.txt(x0-10, yh+13, "hub", 11, INK, "end", 700)
    s.txt(x0-10, yw+15, "nodes", 11, INK, "end", 700)
    s.txt(x0-10, ya+13, "node k", 11, INK, "end", 700)
    for i in range(8):
        s.rect(zp(i*PITCH-ARM), yw, zp(WIN)-zp(0), 22, FILL_RX, RX)
        s.txt(zp(i*PITCH), yw+36, ("k" if i == 0 else f"k+{i}"), 9.5, MUTE, "middle")
    s.rect(zp(-FIRE), yh, zp(CMD)-zp(0), 20, FILL_TX, TX)
    s.txt(zp(CMD)+10, yh+14, f"downlink  {CMD:.1f} ms", 10, TX, "start", 600)
    UL = 100.0
    s.rect(zp(UL-FIRE), ya, zp(ACK)-zp(0), 20, "#dafbe1", "#1a7f37")
    s.txt(zp(UL+ACK/2), ya+34, f"ack {ACK:.1f} ms  at  T0 + ulOffsetUs", 10, "#1a7f37", "middle", 600)
    s.line(zp(0), yh-4, zp(0), ya+24, INK, 1.6)
    s.txt(zp(0), yh-10, "T0", 11, INK, "middle", 700)
    occ = CMD - T_PRE + 20 + ACK
    s.rect(zp(-FIRE), ya+46, zp(occ)-zp(0), 5, WARN, None, 2, .4)
    s.txt(zx0, ya+72, f"hub occupies T0−3.1 … T0+{occ:.0f} ms  →  next servable slot is k+3  "
                      f"→  10 nodes per round", 10.5, WARN, w=600)
    s.line(x0, ya+90, x0+span, ya+90, GRID, 1)

    # ---- row 3 : beacon round ----------------------------------------------
    yb = 432
    s.txt(x0-10, yb+8,  "beacon", 11, INK, "end", 700)
    s.txt(x0-10, yb+22, "round",  11, INK, "end", 700)
    s.rect(px(0), yb, px(BCN)-px(0), 22, FILL_BCN, BCNC)
    s.txt(px(BCN/2), yb-9, "beacon slot", 10, BCNC, "middle", 700)
    for k in range(2, SLOTS):
        s.rect(px(k*PITCH-ARM), yb, px(WIN)-px(0), 22, FILL_RX, RX, op=.5)
    s.rect(px(PITCH-ARM), yb, px(WIN)-px(0), 22, "#ffe0e0", WARN)
    s.txt(px(PITCH)+30, yb+38, "slot 1 blinded by the beacon's own airtime", 9.5, WARN, "start", 600)
    s.txt(x0, yb+62, f"Every M rounds, ALL nodes open one extra window in the beacon slot "
                     f"({BCN:.1f} ms, flat in node count). The 32 private windows sit at 32", 10.5, MUTE)
    s.txt(x0, yb+78, "different phases, so one broadcast cannot reach them all. It needs a position, "
                     "not just a period.", 10.5, MUTE)
    s.save("03-mode-b-round.svg")

# =========================================================== 4. window detail
def d4():
    W, H = 980, 310
    s = S(W, H, "4 — Mode B window: the ±14.08 ms guard falls out of the existing symbol timeout")
    x0, span = 160.0, 690.0
    lo, hi = -ARM-7, -ARM+WIN+9
    px = lambda ms: x0 + (ms-lo)/(hi-lo)*span
    y = 104
    s.rect(px(-ARM), y, px(-ARM+WIN)-px(-ARM), 40, FILL_RX, RX)
    s.txt(px(-ARM+WIN/2), y+24, f"RX_SINGLE  ·  symTimeout 115 sym  ·  {WIN:.2f} ms", 11, RX, "middle", 600)
    s.rect(px(-T_PRE), y-30, px(0)-px(-T_PRE), 18, FILL_TX, TX)
    s.txt(px(-T_PRE/2), y-36, "preamble", 9.5, TX, "middle", 600)
    s.line(px(0), y-46, px(0), y+96, INK, 2)
    s.txt(px(0), y-52, "T0", 12.5, INK, "middle", 700)
    s.line(px(-ARM), y-8, px(-ARM), y+62, MUTE, 1, "3 3")
    s.txt(px(-ARM), y-14, f"ARM at T0 − {ARM:.3f} ms", 10.5, MUTE, "middle", 600)
    s.arrow(px(-GUARD), y+64, px(0), "#1a7f37", f"early {GUARD:.2f} ms")
    s.arrow(px(0), y+84, px(GUARD), "#1a7f37", f"late {GUARD:.2f} ms")
    yb = y + 108
    s.rect(x0-64, yb, span+128, 82, "#f6f8fa", GRID, 4)
    s.txt(x0-48, yb+24, f"G = (N_symtimeout·T_sym − T_detect)/2 = ({WIN:.2f} − {T_DET:.2f})/2 = ±{GUARD:.2f} ms",
          11, INK, w=600, mono=True)
    s.txt(x0-48, yb+46, f"beacon interval ceiling:  {GUARD/20e-3/60:.1f} min at ±20 ppm (unmeasured)   ·   "
                        f"{GUARD/2e-3/60:.0f} min at ±2 ppm (measured).  Operate at half these.", 10.5, MUTE)
    s.txt(x0-48, yb+66, "T_detect ≈ 5 symbols is a rule of thumb, not a datasheet number, and it sets the "
                        "entire late-side margin. Bench item.", 10.5, WARN)
    s.save("04-mode-b-window.svg")

# ============================================================= 5. mode C wake
def d5():
    W, H = 980, 372
    s = S(W, H, "5 — Mode C wake (LoRaWAN Class A): three stages of the same exchange")
    x0, span, TOT = 112.0, 690.0, 30000.0
    px = lambda ms: x0 + ms/TOT*span
    def stage(y, label, total, blocks, note):
        s.txt(x0-10, y+16, label, 11, INK, "end", 700)
        s.rect(x0, y, span, 26, "#f6f8fa", GRID, 3)
        for t, d, c, f, lab in blocks:
            s.rect(px(t), y, max(px(t+d)-px(t), 2.5), 26, f, c)
        for t, d, c, f, lab in blocks:
            if lab: s.txt(px(t+d/2), y+40, lab, 9.5, c, "middle", 600)
        s.line(px(total), y-7, px(total), y+33, WARN, 1.6)
        s.txt(px(total)+8, y+18, f"{total/1000:.1f} s", 11, WARN, w=700)
        s.txt(x0, y+58, note, 10, MUTE)
    stage(72, "today", 28139,
          [(0,919,SLEEP,"#eef1f4","boot"),(1169,120,TX,FILL_TX,"beacon"),
           (4699,120,RX,FILL_RX,"TimeSync"),(4819,20290,SLEEP,"#eef1f4","waiting out a fixed 20 s silence")],
          "73 % of the wake is the node proving a negative")
    stage(186, "after C1", 7700,
          [(0,919,SLEEP,"#eef1f4","boot"),(1169,120,TX,FILL_TX,"beacon"),
           (4699,120,RX,FILL_RX,"TimeSync + sleepOk"),(4819,2881,SLEEP,"#eef1f4","drain")],
          "one proto field: the hub says 'nothing further for you'")
    stage(300, "after C2", 3000,
          [(0,919,SLEEP,"#eef1f4","boot"),(1169,120,TX,FILL_TX,"beacon"),
           (2169,60,RX,FILL_RX,"RX1"),(3169,60,RX,FILL_RX,"RX2")],
          "RX1/RX2 at fixed offsets from the node's own TxDone — no clock agreement needed. "
          "Blocked on the hub half (§5.4).")
    s.txt(x0+span-250, 52, "112  →  31  →  12 awake s/day at a 6 h check-in", 11, INK, w=700)
    s.save("05-mode-c-wake.svg")

# =========================================================== 6. power states
def d6():
    W, H = 980, 430
    s = S(W, H, "6 — Power states: node current through one 1500 ms round")
    x0, span, IMAX = 112.0, 640.0, 13.0
    px = lambda ms: x0 + ms/ROUND*span
    BASE, RXON = 0.8, 11.0
    def trace(y0, h, label, segs, avg, note):
        py = lambda i: y0 + h - min(i, IMAX)/IMAX*h
        s.txt(x0-10, y0+h/2+4, label, 11, INK, "end", 700)
        s.rect(x0, y0, span, h, "#fcfcfd", GRID, 2)
        for lvl, lab in ((BASE, "0.8"), (RXON, "11")):
            s.line(x0, py(lvl), x0+span, py(lvl), GRID, 1, "2 3")
            s.txt(x0-6, py(lvl)+3, lab, 8.5, GRID, "end", 500, mono=True)
        pts = [(0, BASE)]
        for t, dur, i in segs:
            pts += [(t, BASE), (t, i), (t+dur, i), (t+dur, BASE)]
        pts.append((ROUND, BASE))
        d = "M" + " L".join(f"{px(t):.1f},{py(i):.1f}" for t, i in pts)
        for t, dur, i in segs:
            s.rect(px(t), py(i), max(px(t+dur)-px(t), 1.5), y0+h-py(i), FILL_RX, None, 1, .6)
        s.path(d, RX, 1.8)
        s.txt(x0+span+14, y0+h/2-3, avg, 11, INK, w=700, mono=True)
        s.txt(x0+span+14, y0+h/2+14, note, 9.5, MUTE)
    trace(76, 72, "Mode A", [(k*RXI_A+22, WIN, RXON) for k in range(3)], "26.2 mAh/day", "3 windows · 5.9 %")
    trace(180, 72, "Mode B", [(7*PITCH-ARM, WIN, RXON)], "18.4 mAh/day", "1 window · 1.96 %")
    trace(284, 72, "+ beacon", [(0, BCN, RXON), (7*PITCH-ARM, WIN, RXON)], "≈18.4 mAh/day", "beacon rounds only")
    s.txt(x0, 372, "mA on the vertical axis; the trace is the node, not the radio alone.", 10, MUTE)
    yb = 398
    s.txt(x0-10, yb, "provenance", 10.5, INK, "end", 700)
    for i, (lab, tag, c) in enumerate([
            ("1.2 mA interactive average", "MEASURED", "#1a7f37"),
            ("~0.8 mA light-sleep floor", "derived from the above", MUTE),
            ("~11 mA RX-on", "UNVERIFIED (§12.4)", WARN),
            ("deep sleep", "NEVER MEASURED", WARN)]):
        x = x0 + i*205
        s.txt(x, yb, lab, 10, INK, w=600); s.txt(x, yb+14, tag, 9.5, c, w=600)
    s.save("06-power-states.svg")

if __name__ == "__main__":
    print("generating:"); d1(); d2(); d3(); d4(); d5(); d6()
