"""Render the animated figures used in the README.

GitHub shows SVG files through <img>: CSS animation works, script does not, and only
fonts installed on the reader's machine are available. Each figure has one cycle.
Every element gets its own keyframes, written from absolute times in that cycle.

Run: python3 docs/assets/render.py
"""

import math
from pathlib import Path

OUT = Path(__file__).parent

PAPER, PANEL, RULE, DOT = "#f4efe6", "#ebe3d4", "#c9bea9", "#d9cfbe"
INK, SOFT = "#23201b", "#6e675b"
DATA, ACK, CTRL, RETX = "#cf4520", "#2e6a4f", "#2c4a78", "#b47a12"

SERIF = "'Iowan Old Style','Charter','Bitstream Charter','Palatino Linotype',Georgia,serif"
SANS = "'Avenir Next','Segoe UI Variable','Segoe UI','Helvetica Neue',sans-serif"
MONO = "'SF Mono','JetBrains Mono','Cascadia Code',Menlo,Consolas,monospace"
EASE = "cubic-bezier(.22,1,.36,1)"


class Fig:
    def __init__(self, w, h, cycle, label, poster):
        self.w, self.h, self.cycle, self.label, self.poster = w, h, cycle, label, poster
        self.body, self.css, self.n = [], [], 0

    def add(self, *parts):
        self.body.extend(parts)

    def anim(self, frames, ease=True):
        self.n += 1
        name = f"k{self.n}"
        steps = "".join(f"{100 * t / self.cycle:.3f}%{{{css}}}" for t, css in sorted(frames, key=lambda f: f[0]))
        timing = EASE if ease else "linear"
        self.css.append(f"@keyframes {name}{{{steps}}}.{name}{{animation:{name} {self.cycle}s {timing} infinite}}")
        return f"{name} loop"

    def once(self, start, dur=.7, frm="opacity:0;transform:translateY(6px)", to="opacity:1;transform:none"):
        self.n += 1
        name = f"k{self.n}"
        self.css.append(f"@keyframes {name}{{from{{{frm}}}to{{{to}}}}}"
                        f".{name}{{animation:{name} {dur}s {EASE} {start}s both}}")
        return f"{name} once"

    def show(self, start, end, fade=.3):
        c = self.cycle
        return self.anim([(0, "opacity:0"), (start, "opacity:0"), (start + fade, "opacity:1"),
                          (end, "opacity:1"), (min(end + fade, c), "opacity:0"), (c, "opacity:0")], ease=False)

    def move(self, points):
        """points: (t, x, y, opacity). The element is drawn at the origin."""
        first, last = points[0], points[-1]
        frames = [(0, f"transform:translate({first[1]}px,{first[2]}px);opacity:0")]
        frames += [(t, f"transform:translate({x}px,{y}px);opacity:{o}") for t, x, y, o in points]
        frames.append((self.cycle, f"transform:translate({last[1]}px,{last[2]}px);opacity:0"))
        return self.anim(frames)

    def write(self, name):
        css = (f".s{{font-family:{SANS}}}.r{{font-family:{SERIF}}}.m{{font-family:{MONO}}}"
               + "".join(self.css) +
               f"@media (prefers-reduced-motion:reduce){{.loop{{animation-play-state:paused!important;"
               f"animation-delay:-{self.poster}s!important}}.once{{animation:none!important}}}}")
        defs = (f'<defs><pattern id="dots" width="16" height="16" patternUnits="userSpaceOnUse">'
                f'<circle cx="1" cy="1" r=".9" fill="{DOT}"/></pattern>'
                f'<pattern id="hatch" width="7" height="7" patternUnits="userSpaceOnUse" patternTransform="rotate(45)">'
                f'<line x1="0" y1="0" x2="0" y2="7" stroke="{DOT}" stroke-width="1.2"/></pattern></defs>')
        w, h = self.w, self.h
        marks = "".join(f'<path d="M{x} {y + dy * 14}V{y}H{x + dx * 14}" fill="none" stroke="{SOFT}" stroke-width="1"/>'
                        for x, y, dx, dy in [(14, 14, 1, 1), (w - 14, 14, -1, 1), (14, h - 14, 1, -1), (w - 14, h - 14, -1, -1)])
        svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
               f'role="img" aria-label="{self.label}"><style>{css}</style>{defs}'
               f'<rect width="{w}" height="{h}" fill="{PAPER}"/><rect width="{w}" height="{h}" fill="url(#dots)"/>'
               f'{marks}' + "".join(self.body) + "</svg>\n")
        (OUT / name).write_text(svg)


def t(x, y, s, size=13, color=INK, face="s", weight=400, anchor="start", spacing=0, cls=""):
    classes = " ".join(c for c in (face, cls) if c)
    extra = f' letter-spacing="{spacing}"' if spacing else ""
    return (f'<text class="{classes}" x="{x:.1f}" y="{y:.1f}" font-size="{size}" font-weight="{weight}" '
            f'fill="{color}" text-anchor="{anchor}"{extra}>{s}</text>')


def caps(x, y, s, color=SOFT, size=10.5, anchor="start"):
    return t(x, y, s.upper(), size, color, "s", 600, anchor, 1.6)


def rect(x, y, w, h, stroke=INK, fill=PANEL, sw=1.2, dash=""):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="1.5" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{sw}"{d}/>')


def line(x1, y1, x2, y2, color=INK, sw=1.2, dash=""):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{color}" stroke-width="{sw}"{d}/>'


def g(cls, *parts):
    return f'<g class="{cls}">' + "".join(parts) + "</g>"


def token(color, w=22, h=10):
    return f'<rect width="{w}" height="{h}" rx="1" fill="{color}"/>'


def header(f, number, title, sub):
    f.add(caps(40, 50, f"Fig. {number}", DATA), t(40, 80, title, 25, INK, "r", 600),
          t(40, 102, sub, 13.5, SOFT), line(40, 118, f.w - 40, 118, RULE, 1))


def draw_line(f, x1, y1, x2, y2, color, start, end, sw=2, dur=.55):
    """A line that draws itself at `start` and stays until `end`."""
    length = math.hypot(x2 - x1, y2 - y1)
    cls = f.anim([(0, f"stroke-dashoffset:{length:.0f};opacity:0"), (start, f"stroke-dashoffset:{length:.0f};opacity:1"),
                  (start + dur, "stroke-dashoffset:0;opacity:1"), (end, "stroke-dashoffset:0;opacity:1"),
                  (min(end + .4, f.cycle), "stroke-dashoffset:0;opacity:0"), (f.cycle, "opacity:0")])
    return (f'<line class="{cls}" x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{color}" '
            f'stroke-width="{sw}" stroke-dasharray="{length:.0f}"/>')


def head(x, y, direction, color):
    d = 1 if direction > 0 else -1
    return f'<polygon points="{x - 8 * d},{y - 4.5} {x},{y} {x - 8 * d},{y + 4.5}" fill="{color}"/>'


def grow_bar(f, x, y, w, h, color, frames, ease=True):
    cls = f.anim([(t_, f"transform:scaleX({s})") for t_, s in frames], ease)
    return (f'<rect class="{cls}" x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h}" fill="{color}" '
            f'style="transform-box:fill-box;transform-origin:left center"/>')


def leader_row(x0, x1, y, label, value, unit="", value_color=INK):
    unit_w = len(unit) * 6.6 + 7 if unit else 0
    val_x = x1 - unit_w
    lead0, lead1 = x0 + len(label) * 6.9 + 10, val_x - len(value) * 11.2 - 8
    parts = [t(x0, y, label, 13, INK), line(lead0, y - 3, lead1, y - 3, RULE, 1.2, "1 4"),
             t(val_x, y, value, 21, value_color, "r", 600, "end")]
    if unit:
        parts.append(t(x1, y, unit, 11.5, SOFT, anchor="end"))
    return parts


# --------------------------------------------------------------------------------------------- figures


def hero():
    f = Fig(960, 350, 6, "ustack, a TCP/IP stack written from scratch in C", 2.2)
    f.add(g(f.once(.05), caps(48, 66, "A TCP/IP stack · written from scratch in C", DATA, 11.5)))
    f.add(g(f.once(.15), t(46, 140, "ustack", 78, INK, "r", 600, spacing=-1)))
    f.add(g(f.once(.3), t(48, 178, "Linux gives it raw IPv4 packets through a TUN device.", 15.5, SOFT),
            t(48, 200, "IPv4, ICMP, UDP, TCP and HTTP/1.1 above that are all this code.", 15.5, SOFT)))

    f.add(f'<rect x="48" y="248" width="150" height="54" fill="url(#hatch)"/>', rect(48, 248, 150, 54, INK, "none"),
          t(123, 271, "Linux kernel", 14, INK, "r", 600, "middle"), t(123, 290, "curl · wrk · ping", 11.5, SOFT, "m", anchor="middle"),
          rect(428, 248, 140, 54, DATA, PAPER, 1.6), t(498, 271, "ustack", 15, DATA, "r", 700, "middle"),
          t(498, 290, "10.0.0.2", 11.5, SOFT, "m", anchor="middle"),
          line(198, 265, 428, 265, RULE, 1), line(198, 285, 428, 285, RULE, 1), caps(313, 244, "tun0", SOFT, 10, "middle"))
    for i in range(4):
        t0 = i * 1.5
        f.add(g(f.move([(t0, 404, 260, 0), (t0 + .1, 404, 260, 1), (t0 + 1.2, 204, 260, 1), (t0 + 1.3, 204, 260, 0)]),
                token(DATA)))
        f.add(g(f.move([(t0 + .8, 204, 281, 0), (t0 + .9, 204, 281, 1), (t0 + 1.8, 408, 281, 1), (t0 + 1.9, 408, 281, 0)]),
                token(ACK, 16, 8)))

    x0, x1 = 618, 912
    f.add(g(f.once(.35), caps(x0, 66, "Measured against the Linux kernel"), line(x0, 78, x1, 78, INK, 1)))
    rows = [("HTTP requests per second", "1.26M", "4 cores"), ("Download, one core", "10.2", "Gbit/s"),
            ("Download with TSO/GSO", "68.8", "Gbit/s"), ("Randomized simulations", "100,000", "0 failed"),
            ("Fuzzed inputs", "1.6M", "0 crashes"), ("Memory per connection", "10.4", "KB")]
    for i, (label, value, unit) in enumerate(rows):
        y = 112 + i * 36
        f.add(g(f.once(.5 + i * .09), *leader_row(x0, x1, y, label, value, unit), line(x0, y + 13, x1, y + 13, DOT, .8)))
    f.add(g(f.once(1.2), t(x0, 330, "Docker · Linux 6.12 arm64 · median of 5 runs", 11, SOFT)))
    f.write("hero.svg")


def architecture():
    f = Fig(960, 660, 8, "Architecture: four shards, each with its own tun queue, stack and CPU core", 2.9)
    header(f, 1, "The big picture", "One process, N shards. Each shard owns one tun queue, one stack and one CPU core.")
    for y0, y1, a, b in [(136, 196, "Linux", "user space"), (206, 340, "Linux", "kernel"), (352, 600, "ustack", "process")]:
        f.add(caps(40, y0 + 22, a, INK), caps(40, y0 + 38, b), line(40, y1, 920, y1, DOT, .8))
    f.add('<rect x="150" y="206" width="770" height="134" fill="url(#hatch)"/>')
    xs = [150 + k * 192 for k in range(4)]
    for k, app in enumerate(["curl", "wrk", "ping", "nc"]):
        f.add(rect(xs[k] + 40, 146, 90, 34, INK, PAPER), t(xs[k] + 85, 168, app, 13, INK, "m", 600, "middle"))
    f.add(rect(330, 218, 380, 38, INK, PAPER), t(520, 242, "TCP/IP stack · 10.0.0.1", 14, INK, "r", 600, "middle"))
    f.add(rect(150, 266, 750, 66, CTRL, PAPER, 1.4), t(162, 284, "tun0", 13, CTRL, "m", 700),
          t(204, 284, "IFF_MULTI_QUEUE", 10.5, SOFT, "m"))
    for k, x in enumerate(xs):
        f.add(rect(x + 65, 294, 40, 28, CTRL, PANEL, 1), t(x + 85, 313, f"q{k}", 12, CTRL, "m", 600, "middle"),
              line(x + 85, 332, x + 85, 360, RULE, 1),
              rect(x, 360, 170, 226, INK, PAPER, 1.2), caps(x + 12, 381, f"shard {k} · cpu {k}", DATA if k == 0 else INK))
        for j, (name, face) in enumerate([("tun queue fd", "m"), ("IPv4 · ICMP · UDP", "s"), ("TCP · timer wheel", "s"),
                                          ("HTTP · echo", "s")]):
            y = 394 + j * 46
            f.add(rect(x + 16, y, 138, 36, RULE, PANEL, 1), t(x + 85, y + 23, name, 12, INK, face, 500, "middle"))
    f.add(t(925, 305, "①", 15, CTRL, "r", anchor="end"), t(925, 381, "②", 15, DATA, "r", anchor="end"))
    f.add(t(40, 622, "①  The kernel hashes each flow's 4-tuple to one queue, and learns the reply queue from our writes.", 12.5, SOFT),
          t(40, 642, "②  Shards share nothing: no locks, no atomics. A connection never leaves its shard.", 12.5, SOFT))
    for k, x in enumerate(xs):
        src = x + 85
        for rep in range(2):
            t0 = k * .5 + rep * 4
            down = [(t0, src - 4, 178, 0), (t0 + .1, src - 4, 178, 1), (t0 + .5, 516, 224, 1), (t0 + .9, src - 4, 304, 1),
                    (t0 + 1.2, x + 4, 400, 1), (t0 + 2.0, x + 4, 560, 1), (t0 + 2.1, x + 4, 560, 0)]
            f.add(g(f.move(down), f'<rect width="8" height="8" fill="{DATA}"/>'))
            up = [(t0 + 2.1, x + 158, 560, 0), (t0 + 2.2, x + 158, 560, 1), (t0 + 3.0, x + 158, 400, 1),
                  (t0 + 3.3, src + 2, 304, 1), (t0 + 3.6, 520, 230, 1), (t0 + 3.9, src + 2, 180, 1), (t0 + 4.0, src + 2, 180, 0)]
            f.add(g(f.move(up), f'<rect width="8" height="8" fill="none" stroke="{ACK}" stroke-width="2"/>'))
    f.write("architecture.svg")


def packet_path():
    f = Fig(960, 400, 10, "The life of one received packet inside ustack", 3.6)
    header(f, 2, "Life of a packet", "One segment in. Zero heap allocations. One coalesced ACK out.")
    rail, xs = 178, [70 + i * 135 for i in range(7)]
    stations = [("readv()", "≤ 256 per batch", "m"), ("IPv4", "length · checksum · address", "s"),
                ("TCP checksum", "pseudo-header", "s"), ("lookup", "hot cache, then SipHash", "s"),
                ("RFC 9293", "PAWS · window · state", "s"), ("ring copy", "one copy, final offset", "s"),
                ("on_readable()", "the app runs", "m")]
    f.add(line(xs[0], rail, xs[-1], rail, INK, 1.2))
    for i, (x, (name, note, face)) in enumerate(zip(xs, stations)):
        t0 = .4 + i * .9
        lit = f.anim([(0, f"fill:{PAPER}"), (t0, f"fill:{PAPER}"), (t0 + .1, f"fill:{DATA}"), (t0 + .85, f"fill:{DATA}"),
                      (t0 + 1.1, f"fill:{PAPER}"), (f.cycle, f"fill:{PAPER}")], ease=False)
        f.add(caps(x, 150, f"{i + 1:02d}", SOFT, 10, "middle"),
              f'<rect class="{lit}" x="{x - 7}" y="{rail - 7}" width="14" height="14" stroke="{INK}" stroke-width="1.2"/>',
              t(x, rail + 32, name, 13.5, INK, face, 600, "middle"), t(x, rail + 50, note, 11, SOFT, anchor="middle"))
    path = [(.4 + i * .9, x - 13, rail - 26, 1) for i, x in enumerate(xs)]
    f.add(g(f.move([(.2, xs[0] - 13, rail - 26, 0)] + path + [(6.6, xs[-1] - 13, rail - 26, 0)]), token(DATA, 26, 10)))

    back, rx = 318, [xs[6], xs[5], xs[4]]
    f.add(line(xs[6], rail + 62, xs[6], back, RULE, 1, "3 3"), line(rx[-1], back, rx[0], back, ACK, 1.2))
    for i, (x, (name, note, face)) in enumerate(zip(rx, [("stack_flush()", "per dirty connection", "m"),
                                                        ("ACK", "coalesced, with SACK", "s"),
                                                        ("writev()", "back to the kernel", "m")])):
        t0 = 7.0 + i * .8
        lit = f.anim([(0, f"fill:{PAPER}"), (t0, f"fill:{PAPER}"), (t0 + .1, f"fill:{ACK}"), (t0 + .75, f"fill:{ACK}"),
                      (t0 + 1.0, f"fill:{PAPER}"), (f.cycle, f"fill:{PAPER}")], ease=False)
        f.add(f'<rect class="{lit}" x="{x - 7}" y="{back - 7}" width="14" height="14" stroke="{ACK}" stroke-width="1.2"/>',
              t(x, back + 30, name, 13.5, INK, face, 600, "middle"), t(x, back + 48, note, 11, SOFT, anchor="middle"))
    path = [(7.0 + i * .8, x - 11, back - 24, 1) for i, x in enumerate(rx)]
    f.add(g(f.move([(6.8, rx[0] - 11, back - 24, 0)] + path + [(9.4, rx[-1] - 11, back - 24, 0)]), token(ACK, 22, 9)))
    f.add(t(70, 300, "Zero heap allocations per packet.", 17, INK, "r", 600),
          t(70, 322, "The core never calls the OS.", 12.5, SOFT), t(70, 340, "The driver gives it bytes and a clock.", 12.5, SOFT))
    f.write("packet-path.svg")


def handshake():
    f = Fig(960, 680, 15, "One TCP connection between curl and ustack, from SYN to CLOSED", 12)
    header(f, 3, "One TCP connection, start to end", "Each arrow is a real segment. Each tag is an RFC 9293 state inside the stack.")
    lx, rx = 330, 710
    f.add(t(lx, 156, "curl", 18, INK, "r", 600, "middle"), t(lx, 174, "Linux kernel TCP", 11.5, SOFT, anchor="middle"),
          t(rx, 156, "ustack", 18, DATA, "r", 600, "middle"), t(rx, 174, ":80", 11.5, SOFT, "m", anchor="middle"),
          line(lx, 186, lx, 640, INK, 1), line(rx, 186, rx, 640, INK, 1))
    rows = [(1, "SYN", CTRL, "SYN_SENT", "SYN_RCVD"), (-1, "SYN-ACK", CTRL, "", ""), (1, "ACK", ACK, "ESTABLISHED", "ESTABLISHED"),
            (1, "GET /bytes/1000000", DATA, "", ""), (-1, "690 segments · 1 MiB window", DATA, "", ""),
            (1, "ACK + SACK", ACK, "", ""), (1, "FIN", CTRL, "FIN_WAIT_1", "CLOSE_WAIT"), (-1, "ACK", ACK, "FIN_WAIT_2", ""),
            (-1, "FIN", CTRL, "TIME_WAIT", "LAST_ACK"), (1, "ACK", ACK, "", "CLOSED")]
    ys = [214 + i * 44 for i in range(len(rows))]
    for name, a, b in [("handshake", 0, 2), ("transfer", 3, 5), ("teardown", 6, 9)]:
        f.add(f'<path d="M108 {ys[a] - 12}h-8v{ys[b] - ys[a] + 24}h8" fill="none" stroke="{RULE}" stroke-width="1.2"/>',
              caps(94, (ys[a] + ys[b]) / 2 + 4, name, SOFT, 10, "end"))
    end = f.cycle - 1.4
    for i, (d, label, color, ls, rs) in enumerate(rows):
        y, t0 = ys[i], .5 + i * 1.05
        x1, x2 = (lx, rx) if d > 0 else (rx, lx)
        f.add(draw_line(f, x1, y, x2 - 8 * d, y, color, t0, end))
        f.add(g(f.show(t0 + .45, end), head(x2, y, d, color), t((lx + rx) / 2, y - 8, label, 12.5, color, "s", 600, "middle")))
        for state, x, anchor in [(ls, lx - 12, "end"), (rs, rx + 12, "start")]:
            if state:
                w = len(state) * 7.4 + 14
                bx = x - w if anchor == "end" else x
                f.add(g(f.show(t0 + .3, end), rect(bx, y - 11, w, 20, CTRL, PAPER, 1),
                        t(bx + w / 2, y + 3.5, state, 10.5, CTRL, "m", 600, "middle")))
    f.add(t(40, 662, "TIME_WAIT keeps the port for 2·MSL, so a late duplicate cannot enter a new connection.", 12.5, SOFT))
    f.write("handshake.svg")


def sector(cx, cy, r0, r1, a0, a1):
    def p(r, a):
        return cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))
    (x0, y0), (x1, y1), (x2, y2), (x3, y3) = p(r1, a0), p(r1, a1), p(r0, a1), p(r0, a0)
    return f"M{x0:.1f} {y0:.1f}A{r1} {r1} 0 0 1 {x1:.1f} {y1:.1f}L{x2:.1f} {y2:.1f}A{r0} {r0} 0 0 0 {x3:.1f} {y3:.1f}Z"


def reassembly():
    f = Fig(960, 540, 12, "Out-of-order segments land at their final offset in a ring buffer", 7.6)
    header(f, 4, "Reassembly without a queue", "The receive buffer is a ring. Out-of-order data goes straight to its final offset.")
    cx, cy, r0, r1 = 300, 330, 88, 166

    def angle(k):
        return -90 + k * 45

    f.add(f'<circle cx="{cx}" cy="{cy}" r="{r1 + 22}" fill="none" stroke="{DOT}" stroke-width="1"/>')
    for i in range(8):
        f.add(f'<path d="{sector(cx, cy, r0, r1, angle(i) + .8, angle(i + 1) - .8)}" fill="{PANEL}" stroke="{RULE}" stroke-width="1"/>')
    for seg, t0, color in [(1, .4, DATA), (2, 1.4, DATA), (4, 2.6, CTRL), (5, 3.4, CTRL), (6, 4.2, CTRL), (3, 6.0, RETX)]:
        a = math.radians(angle(seg - 1) + 22.5)
        mx, my = cx + 128 * math.cos(a), cy + 128 * math.sin(a)
        fill = f.anim([(0, "opacity:0"), (t0 + .7, "opacity:0"), (t0 + .9, "opacity:1"), (9.2, "opacity:1"),
                       (9.4 + seg * .15, "opacity:.16"), (11.4, "opacity:.16"), (11.7, "opacity:0"), (12, "opacity:0")])
        f.add(f'<path class="{fill}" d="{sector(cx, cy, r0, r1, angle(seg - 1) + .8, angle(seg) - .8)}" fill="{color}"/>')
        f.add(g(f.move([(t0, mx - 11, 140, 0), (t0 + .1, mx - 11, 140, 1), (t0 + .8, mx - 11, my - 5, 1),
                        (t0 + .9, mx - 11, my - 5, 0)]), token(color)))
    for i in range(8):
        a = math.radians(angle(i) + 22.5)
        f.add(t(cx + 128 * math.cos(a), cy + 128 * math.sin(a) + 5, str(i + 1), 15, INK, "r", 600, "middle"))
    hand = f.anim([(0, "transform:rotate(-90deg)"), (1.0, "transform:rotate(-90deg)"), (1.3, "transform:rotate(-45deg)"),
                   (2.0, "transform:rotate(-45deg)"), (2.3, "transform:rotate(0deg)"), (6.9, "transform:rotate(0deg)"),
                   (7.6, "transform:rotate(180deg)"), (11.5, "transform:rotate(180deg)"), (12, "transform:rotate(270deg)")])
    f.add(f'<g class="{hand}" style="transform-origin:{cx}px {cy}px">{line(cx, cy, cx + r1 + 30, cy, INK, 2.2)}'
          f'<circle cx="{cx + r1 + 30}" cy="{cy}" r="4" fill="{INK}"/></g>',
          f'<circle cx="{cx}" cy="{cy}" r="34" fill="{PAPER}" stroke="{INK}" stroke-width="1.2"/>',
          t(cx, cy - 1, "rcv_nxt", 11, INK, "m", 600, "middle"), t(cx, cy + 14, "hand", 10, SOFT, anchor="middle"))
    arc_r = r1 + 12
    a0, a1 = math.radians(angle(3) + 2), math.radians(angle(6) - 2)
    arc = (f"M{cx + arc_r * math.cos(a0):.1f} {cy + arc_r * math.sin(a0):.1f}"
           f"A{arc_r} {arc_r} 0 0 1 {cx + arc_r * math.cos(a1):.1f} {cy + arc_r * math.sin(a1):.1f}")
    f.add(g(f.show(4.7, 6.6), f'<path d="{arc}" fill="none" stroke="{CTRL}" stroke-width="3"/>',
            t(cx + 196, cy + 132, "SACK 4–6", 13, CTRL, "s", 700)))
    f.add(caps(cx - 150, 136, "from the network"), line(cx - 150, 146, cx + 150, 146, RULE, 1, "2 4"))
    steps = [("In-order data arrives.", "rcv_nxt moves one segment at a time.", .2, 2.5),
             ("Segment 3 is missing.", "4, 5 and 6 land at their own offsets. The ACK says SACK 4–6.", 2.5, 5.8),
             ("The retransmission of 3 arrives.", "rcv_nxt sweeps across four segments at once.", 5.8, 8.4),
             ("The app reads.", "The space goes back into the advertised window.", 8.4, 11.4)]
    for i, (title_s, body, a, b) in enumerate(steps):
        y = 196 + i * 76
        on = f.anim([(0, "opacity:.3"), (a, "opacity:.3"), (a + .3, "opacity:1"), (b, "opacity:1"),
                     (b + .3, "opacity:.3"), (f.cycle, "opacity:.3")], ease=False)
        f.add(g(on, t(560, y + 6, f"{i + 1}", 30, DATA, "r", 600), t(596, y - 6, title_s, 15, INK, "r", 600),
                t(596, y + 14, body, 12, SOFT)))
    f.add(t(560, 494, "No segment list and no extra copy.", 12, SOFT),
          t(560, 512, "SACK ranges live in a small sorted interval set.", 12, SOFT))
    f.write("reassembly.svg")


def rack():
    f = Fig(960, 680, 12, "RACK detects a lost segment by time, not by counting duplicate ACKs", 9)
    header(f, 5, "RACK: loss detection by time",
           "A segment is lost when a later one is delivered and it is older than RTT + reorder window.")
    lx, rx, fl = 330, 740, 70
    f.add(t(lx, 156, "ustack", 18, DATA, "r", 600, "middle"), t(lx, 174, "sender", 11.5, SOFT, anchor="middle"),
          t(rx, 156, "Linux", 18, INK, "r", 600, "middle"), t(rx, 174, "receiver", 11.5, SOFT, anchor="middle"),
          line(lx, 186, lx, 640, INK, 1), line(rx, 186, rx, 640, INK, 1))
    end = f.cycle - 1

    def arrow(t0, y, color, label, back=False, lost=False):
        x1, x2 = (rx, lx) if back else (lx, rx)
        y2 = y + fl
        if lost:
            x2, y2 = (lx + rx) / 2, y + fl / 2
        f.add(draw_line(f, x1, y, x2, y2, color, t0, end, 1.8))
        tx, anchor = (rx + 12, "start") if back else (lx - 12, "end")
        f.add(g(f.show(t0, end), t(tx, y + 4, label, 12, color, "s", 600, anchor)))
        if lost:
            f.add(g(f.show(t0 + .55, end), t(x2 + 8, y2 + 5, "✕  lost", 13, DATA, "s", 700)))

    for i in range(6):
        arrow(.3 + i * .3, 200 + i * 20, DATA, f"segment {i + 1}", lost=(i == 2))
    arrow(2.6, 200 + 3 * 20 + fl, ACK, "ACK 3 · SACK 4", back=True)
    arrow(3.2, 200 + 5 * 20 + fl, ACK, "ACK 3 · SACK 4–6", back=True)
    by0, by1 = 240, 200 + 5 * 20 + 2 * fl
    bx = lx - 100
    f.add(g(f.show(4.0, end), f'<path d="M{bx + 8} {by0}h-8V{by1}h{lx - bx - 4}" fill="none" stroke="{RETX}" stroke-width="1.6"/>',
            t(bx - 10, (by0 + by1) / 2 + 4, "age of segment 3", 12, RETX, "s", 600, "end"),
            t(bx - 10, (by0 + by1) / 2 + 22, "> RTT + reo_wnd", 12, RETX, "m", 600, "end")))
    f.add(g(f.show(4.6, end), t(lx + 16, by1 + 4, "→ mark segment 3 LOST", 13, RETX, "s", 700)))
    ry = by1 + 40
    arrow(5.4, ry, RETX, "segment 3 again")
    arrow(6.4, ry + fl, ACK, "ACK 7", back=True)
    f.add(t(40, 664, "No duplicate-ACK counting. A lost retransmission is detected the same way: by time.", 12.5, SOFT))
    f.write("rack.svg")


def tlp():
    f = Fig(960, 420, 11, "A tail loss probe recovers in milliseconds instead of waiting for the RTO", 9.8)
    header(f, 6, "TLP: recover a lost tail in ~2 ms, not 200 ms",
           "The last segment has no later segment to reveal its loss. A probe makes the receiver answer.")
    x0, x1 = 200, 900
    for y, label, color in [(190, "with TLP", ACK), (290, "RTO only", SOFT)]:
        f.add(line(x0, y, x1, y, RULE, 1), caps(x0 - 24, y + 4, label, color, 11, "end"))
        f.add(g(f.show(.4, f.cycle - 1), f'<circle cx="{x0 + 10}" cy="{y}" r="6" fill="{DATA}"/>',
                t(x0 + 10, y - 16, "last segment lost", 12, DATA, "s", 600, "middle")))
    probe, ack = 380, 470
    f.add(grow_bar(f, x0 + 10, 202, probe - x0 - 10, 8, ACK, [(0, 0), (.8, 0), (2.4, 1), (f.cycle - 1, 1), (f.cycle, 0)]),
          g(f.show(1.0, f.cycle - 1), t(x0 + 10, 230, "PTO = 2·SRTT + 2 ms", 12, ACK, "m", 600)),
          g(f.show(2.4, f.cycle - 1), f'<circle cx="{probe}" cy="190" r="6" fill="{RETX}"/>',
            t(probe, 174, "resend last segment", 12, RETX, "s", 600, "middle")),
          g(f.show(3.1, f.cycle - 1), f'<circle cx="{ack}" cy="190" r="6" fill="{ACK}"/>',
            t(ack + 16, 195, "duplicate → Linux ACKs at once → recovered", 12.5, ACK, "s", 700)))
    f.add(grow_bar(f, x0 + 10, 302, x1 - x0 - 10, 8, RULE, [(0, 0), (.8, 0), (8.6, 1), (f.cycle - 1, 1), (f.cycle, 0)], False),
          g(f.show(8.6, f.cycle - 1), t(x1, 330, "the RTO fires at 200 ms", 12, SOFT, "s", 600, "end")))
    f.add(line(40, 356, 920, 356, RULE, 1),
          t(40, 382, "Found with tcpdump: a probe with new data is in order, so Linux can hold its ACK for up to 200 ms.", 12.5, INK),
          t(40, 402, "ustack probes with a duplicate.  At 5% loss: 27.0 s → 15.7 s.  At 1% loss: 1.06 s → 0.47 s.",
            12.5, DATA, "s", 600))
    f.write("tlp.svg")


def tso():
    f = Fig(960, 420, 6, "Segmentation offload: one write per 64 KB instead of one per 1.5 KB", 3.4)
    header(f, 7, "Offload: fewer, bigger writes", "With virtio_net_hdr, one write() carries up to 44 segments. The kernel splits them.")
    lx0, lx1 = 220, 640
    for y, name, note, n, w, color, gbps in [(185, "MTU 1500", "one write() per 1,448 B", 12, 22, DATA, 10.2),
                                             (300, "TSO / GSO", "one write() per ~64 KB", 2, 150, CTRL, 68.8)]:
        f.add(t(40, y - 4, name, 15, INK, "r", 600), t(40, y + 14, note, 11.5, SOFT), line(lx0, y + 24, lx1, y + 24, RULE, 1))
        step = f.cycle / n
        for i in range(n):
            t0 = i * step
            f.add(g(f.move([(t0, lx0, y - 10, 0), (t0 + .05, lx0, y - 10, 1), (t0 + 3, lx1 - w, y - 10, 1),
                            (t0 + 3.05, lx1 - w, y - 10, 0)]), token(color, w, 20)))
            tx = lx0 + 4 + (lx1 - lx0 - 8) * t0 / f.cycle
            f.add(g(f.show(t0, f.cycle - .35, .08), line(tx, y + 18, tx, y + 30, color, 2)))
        f.add(caps(lx0, y + 46, f"{n} write() calls in the same time", SOFT, 9.5))
        f.add(t(690, y + 6, f"{gbps}", 30, color, "r", 600), t(690 + len(str(gbps)) * 16 + 6, y + 6, "Gbit/s", 12, SOFT),
              grow_bar(f, 690, y + 16, 220 * gbps / 68.8, 6, color, [(0, 0), (1.6, 1), (f.cycle - .4, 1), (f.cycle, 0)]))
    f.add(t(40, 400, "Same TCP code. The receive path also takes GRO super-segments and trusts NEEDS_CSUM. 6.7× the throughput.",
            12.5, SOFT))
    f.write("tso.svg")


def simulation():
    f = Fig(960, 500, 12, "Deterministic simulation: two stacks over a hostile, seeded virtual network", 4.6)
    header(f, 8, "How we know it works",
           "Two stacks in one process. Virtual time. A seeded link that loses, duplicates, corrupts and reorders.")
    f.add(rect(40, 150, 150, 120, INK, PAPER), t(115, 205, "stack A", 17, INK, "r", 600, "middle"),
          t(115, 225, "client", 11.5, SOFT, anchor="middle"),
          rect(770, 150, 150, 120, INK, PAPER), t(845, 205, "stack B", 17, INK, "r", 600, "middle"),
          t(845, 225, "echo server", 11.5, SOFT, anchor="middle"),
          '<rect x="230" y="150" width="500" height="120" fill="url(#hatch)"/>', rect(230, 150, 500, 120, RETX, "none", 1.2, "5 4"),
          caps(244, 170, "seeded link", RETX))
    for i, label in enumerate(["seed 1", "seed 2", "seed 3", "…", "seed 100,000"]):
        f.add(g(f.show(i * 2.4, i * 2.4 + 2.1, .15), t(716, 170, label, 11.5, SOFT, "m", 600, "end")))
    y = 205
    for kind, color, t0 in [(None, DATA, .3), ("lost", DATA, 1.5), ("duplicated", DATA, 2.9),
                            ("one bit flipped → checksum rejects it", RETX, 4.3),
                            ("delayed → arrives after the next one", CTRL, 5.9), (None, DATA, 6.6)]:
        if kind == "lost":
            pts = [(t0, 200, y, 0), (t0 + .1, 200, y, 1), (t0 + .8, 470, y, 1), (t0 + 1.5, 470, y + 50, 0)]
        elif kind and kind.startswith("delayed"):
            pts = [(t0, 200, y, 0), (t0 + .1, 200, y, 1), (t0 + .6, 470, y - 32, 1), (t0 + 1.7, 470, y - 32, 1),
                   (t0 + 2.4, 744, y, 1), (t0 + 2.5, 744, y, 0)]
        elif kind and kind.startswith("one bit"):
            pts = [(t0, 200, y, 0), (t0 + .1, 200, y, 1), (t0 + 1.4, 744, y, 1), (t0 + 1.8, 744, y, 0)]
        else:
            pts = [(t0, 200, y, 0), (t0 + .1, 200, y, 1), (t0 + 1.4, 744, y, 1), (t0 + 1.5, 744, y, 0)]
        f.add(g(f.move(pts), token(color, 26, 12)))
        if kind == "duplicated":
            f.add(g(f.move([(t0 + .7, 470, y, 0), (t0 + .75, 470, y + 26, 1), (t0 + 1.45, 744, y + 26, 1),
                            (t0 + 1.55, 744, y + 26, 0)]), token(color, 26, 12)))
        if kind:
            f.add(g(f.show(t0 + .4, t0 + 1.1), t(480, 296, kind, 12.5, color, "s", 700, "middle")))
    f.add(caps(40, 340, "Checked after every event", INK), line(40, 350, 440, 350, INK, 1))
    for i, c in enumerate(["the send queue has no gaps", "SACK and loss counters match a recount",
                           "out-of-order ranges stay sorted", "every echoed byte matches what was sent"]):
        on = f.anim([(0, "opacity:.4"), (1 + i * 2.4, "opacity:.4"), (1.3 + i * 2.4, "opacity:1"), (2.6 + i * 2.4, "opacity:1"),
                     (3.0 + i * 2.4, "opacity:.4"), (f.cycle, "opacity:.4")], ease=False)
        f.add(g(on, t(40, 378 + i * 26, "✓", 14, ACK, "s", 700), t(62, 378 + i * 26, c, 13, INK)))
    f.add(caps(520, 340, "One sweep, about two minutes", INK), line(520, 350, 920, 350, INK, 1))
    for i, (label, value) in enumerate([("randomized connections", "100,000"), ("data echoed and verified", "72.7 GB"),
                                        ("segments exchanged", "80.2M"), ("failures", "0")]):
        f.add(*leader_row(520, 920, 378 + i * 26, label, value, value_color=DATA if value == "0" else INK))
    f.write("simulation.svg")


if __name__ == "__main__":
    for render in (hero, architecture, packet_path, handshake, reassembly, rack, tlp, tso, simulation):
        render()
    print("rendered 9 figures into", OUT)
