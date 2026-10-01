#!/usr/bin/env python3
"""Generate the MTL architecture diagram proposals as standalone SVG files.

One layout engine, many themes. Each theme sets the colors, the spacing, the
label style, the chip style, and the glow. Text widths come from the DejaVu
fonts (wider than Intel Clear, Inter or Segoe UI), so a label that fits here
fits in every browser.

    python3 gen_arch.py            # write every proposal next to this file
"""

import os
from xml.sax.saxutils import escape

try:
    from PIL import ImageFont
except ImportError:
    ImageFont = None

OUT_DIR = os.path.dirname(os.path.abspath(__file__))

SANS = "'Intel One Display','Intel Clear',Inter,'Segoe UI',Roboto,'Helvetica Neue',Arial,sans-serif"
MONO = "'JetBrains Mono','Cascadia Code','Fira Code',Consolas,'DejaVu Sans Mono',monospace"

_FONT_FILES = {
    ("sans", True): "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    ("sans", False): "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ("mono", True): "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    ("mono", False): "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
}
_fonts = {}


def text_w(s, size, bold=True, mono=False, spacing=0.0):
    key = ("mono" if mono else "sans", bold, size)
    if ImageFont is not None and key not in _fonts:
        try:
            _fonts[key] = ImageFont.truetype(_FONT_FILES[key[:2]], round(size * 4))
        except OSError:
            _fonts[key] = None
    font = _fonts.get(key)
    if font is not None:
        w = font.getlength(s) / 4
    else:
        w = len(s) * size * (0.64 if bold else 0.58)
    return w + spacing * max(len(s) - 1, 0)


# --------------------------------------------------------------------------
# Color helpers
# --------------------------------------------------------------------------


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i : i + 2], 16) for i in (0, 2, 4))


def hexc(c):
    return "#%02x%02x%02x" % tuple(max(0, min(255, round(v))) for v in c)


def mix(a, b, t):
    ra, rb = rgb(a), rgb(b)
    return hexc([ra[i] + (rb[i] - ra[i]) * t for i in range(3)])


# --------------------------------------------------------------------------
# Diagram content
# --------------------------------------------------------------------------

APPS = [
    ("Video App", None),
    ("Audio App", None),
    ("Ancillary App", None),
    ("Fast Metadata App", None),
    ("3rd-party Plugins", "OBS · FFmpeg · GStreamer"),
]
APIS = ["Frames", "RTP Passthrough", "Datagram"]
PIPELINE = [("ST 2110-20", "Conversion Plugin"), ("ST 2110-22", "Codec Plugin")]
COLOR_CONV = ("Color Format Conversion", "Accelerated by SIMD (AVX512)")
ST2110 = [
    ("ST 2110-20", "Uncompressed Video"),
    ("ST 2110-22", "Compressed Video"),
    ("ST 2110-30/31", "(Un)compressed Audio"),
    ("ST 2110-40", "Ancillary Data"),
    ("ST 2110-41", "Fast Metadata"),
]
CTRL = ["IGMP", "PTP", "DHCP", "ARP", "RTCP"]
MGMT = [
    ("Pacing", "TSN · RL · TSC"),
    ("Tasklet Scheduler", None),
    ("Flow Director / RSS", None),
    ("PTP Timesync", None),
    ("DMA Memcpy Helper", None),
]
DAL = [
    ("Unified Tx & Rx Burst Interface", None),
    ("Tx & Rx Queue Management", None),
    ("Mbuf Pool / Ring Management", None),
    ("Shared Queue Management", None),
    ("Virtio-user Exception Path", None),
]
NIC_HW = [
    ("Intel E810 / E830 / E835", None),
    ("Intel XL710", None),
    ("Intel I225 / I226", None),
    ("X540-AT2 / X550T", None),
    ("Others", None),
]
DMA_HW = [("DSA (IDXD)", None), ("CBDMA (IOAT)", None)]
PLATFORMS = [("Linux", "Bare metal / VM / Container"), ("Windows", "Bare metal / VM")]


# --------------------------------------------------------------------------
# Renderer
# --------------------------------------------------------------------------


class Svg:
    def __init__(self, t):
        self.t = t
        self.defs = []
        self.body = []
        self._ids = {}
        self.dark = t["dark"]

    # -- defs ---------------------------------------------------------------
    def _id(self, kind, *parts):
        key = (kind,) + parts
        if key not in self._ids:
            self._ids[key] = "%s%d" % (kind, len(self._ids))
            return self._ids[key], True
        return self._ids[key], False

    def vgrad(self, stops):
        """stops: list of (offset, color, opacity)."""
        gid, new = self._id("g", tuple(stops))
        if new:
            s = "".join(
                '<stop offset="%s" stop-color="%s" stop-opacity="%.3f"/>' % (o, c, a)
                for o, c, a in stops
            )
            self.defs.append(
                '<linearGradient id="%s" x1="0" y1="0" x2="0" y2="1">%s</linearGradient>'
                % (gid, s)
            )
        return "url(#%s)" % gid

    def marker(self, color, reverse=False):
        mid, new = self._id("m", color, reverse)
        if new:
            path = "M10,0 L0,5 L10,10 z" if reverse else "M0,0 L10,5 L0,10 z"
            self.defs.append(
                '<marker id="%s" viewBox="0 0 10 10" refX="%d" refY="5" '
                'markerWidth="11" markerHeight="11" orient="auto" markerUnits="userSpaceOnUse">'
                '<path d="%s" fill="%s"/></marker>'
                % (mid, 0 if reverse else 10, path, color)
            )
        return "url(#%s)" % mid

    # -- primitives ---------------------------------------------------------
    def rect(self, x, y, w, h, r, fill="none", stroke=None, sw=1.0, opacity=None,
             stroke_opacity=None, dash=None, filt=None, fill_opacity=None):
        a = ['x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="%.1f"' % (x, y, w, h, r)]
        a.append('fill="%s"' % fill)
        if fill_opacity is not None:
            a.append('fill-opacity="%.3f"' % fill_opacity)
        if stroke:
            a.append('stroke="%s" stroke-width="%.2f"' % (stroke, sw))
            if stroke_opacity is not None:
                a.append('stroke-opacity="%.3f"' % stroke_opacity)
        if dash:
            a.append('stroke-dasharray="%s"' % dash)
        if opacity is not None:
            a.append('opacity="%.3f"' % opacity)
        if filt:
            a.append('filter="url(#%s)"' % filt)
        self.body.append("<rect %s/>" % " ".join(a))

    def text(self, x, y, s, size, color, bold=True, anchor="middle", mono=False,
             spacing=0.0, opacity=None, weight=None):
        fam = MONO if mono else SANS
        wt = weight or (700 if bold else 500)
        extra = ' letter-spacing="%.2f"' % spacing if spacing else ""
        if opacity is not None:
            extra += ' fill-opacity="%.3f"' % opacity
        self.body.append(
            '<text x="%.1f" y="%.1f" font-family="%s" font-size="%.1f" font-weight="%d" '
            'fill="%s" text-anchor="%s"%s>%s</text>'
            % (x, y, fam, size, wt, color, anchor, extra, escape(s))
        )

    def line(self, x1, y1, x2, y2, color, sw, start=False, end=True, opacity=None, filt=None):
        a = 'x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="%.2f" stroke-linecap="round"' % (
            x1, y1, x2, y2, color, sw)
        if end:
            a += ' marker-end="%s"' % self.marker(color)
        if start:
            a += ' marker-start="%s"' % self.marker(color, True)
        if opacity is not None:
            a += ' opacity="%.3f"' % opacity
        if filt:
            a += ' filter="url(#%s)"' % filt
        self.body.append("<line %s/>" % a)

    def out(self, w, h):
        return (
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
            'role="img" aria-label="Media Transport Library architecture">\n'
            "<title>Media Transport Library architecture</title>\n"
            "<defs>\n%s\n</defs>\n%s\n</svg>\n"
            % (w, h, w, h, "\n".join(self.defs), "\n".join(self.body))
        )


# --------------------------------------------------------------------------
# Text fitting
# --------------------------------------------------------------------------


def wrap(s, size, max_w, bold=True, mono=False):
    """Return (lines, size) that fit max_w: one line, a balanced two-line
    split, or a smaller font."""
    if "\n" in s:
        lines = s.split("\n")
    elif text_w(s, size, bold, mono) <= max_w:
        return [s], size
    else:
        words = s.split(" ")
        best = None
        for i in range(1, len(words)):
            a, b = " ".join(words[:i]), " ".join(words[i:])
            m = max(text_w(a, size, bold, mono), text_w(b, size, bold, mono))
            if best is None or m < best[0]:
                best = (m, [a, b])
        lines = best[1] if best else [s]
    while size > 9 and max(text_w(l, size, bold, mono) for l in lines) > max_w:
        size -= 0.5
    return lines, size


class Lay:
    """Layout engine. All sizes come from the theme."""

    def __init__(self, t):
        self.t = t
        self.s = Svg(t)
        self.g = t["gap"]
        self.pad = t["pad"]
        self.r = t["radius"]
        self.fs = t["chip_font"]
        self.sub_fs = t["chip_font"] - 2.5
        self.lh = 1.22
        self.mono_labels = t.get("mono_labels", False)
        self._setup_filters()

    # -- filters ------------------------------------------------------------
    def _setup_filters(self):
        t, d = self.t, self.s.defs
        if t["glow"]:
            d.append(
                '<filter id="glow" x="-10%%" y="-30%%" width="120%%" height="160%%">'
                '<feGaussianBlur stdDeviation="%.1f" result="b"/>'
                '<feMerge><feMergeNode in="b"/><feMergeNode in="b"/></feMerge></filter>'
                % t["glow"]
            )
            d.append(
                '<filter id="glowS" x="-20%%" y="-40%%" width="140%%" height="180%%">'
                '<feGaussianBlur stdDeviation="%.1f"/></filter>' % (t["glow"] * 0.6)
            )
        if t.get("shadow"):
            d.append(
                '<filter id="shadow" x="-10%%" y="-10%%" width="120%%" height="140%%">'
                '<feDropShadow dx="0" dy="%s" stdDeviation="%s" flood-color="%s" flood-opacity="%s"/></filter>'
                % t["shadow"]
            )

    # -- color roles ----------------------------------------------------------
    def col(self, key):
        return self.t["c"][key]

    def label_color(self, key):
        c = self.col(key)
        return mix(c, "#ffffff", self.t.get("label_lift", 0.35)) if self.s.dark else mix(c, "#000000", 0.25)

    # -- panels -------------------------------------------------------------
    def panel(self, x, y, w, h, key, level="band", dashed=False):
        """A container: the MTL frame, a layer band, or a group."""
        s, t = self.s, self.t
        c = self.col(key)
        r = self.r + (4 if level == "outer" else 0)
        sw = t["stroke"] * (1.4 if level == "outer" else 1.0)
        dash = "6 5" if dashed else None
        if s.dark:
            a_top, a_bot = t["panel_alpha"][level]
            fill = s.vgrad([(0, c, a_top), (1, c, a_bot)])
            if t.get("panel_base"):
                s.rect(x, y, w, h, r, fill=t["panel_base"])
            if t["glow"] and not dashed and level != "group":
                s.rect(x, y, w, h, r, stroke=c, sw=sw * 2, filt="glow",
                       stroke_opacity=t.get("glow_alpha", 0.55))
            s.rect(x, y, w, h, r, fill=fill, stroke=c, sw=sw,
                   stroke_opacity=t["panel_stroke_alpha"][level], dash=dash)
            if t.get("highlight") and not dashed:
                s.rect(x + r, y + 0.8, w - 2 * r, 1.2, 0.6, fill="#ffffff", fill_opacity=t["highlight"])
        else:
            a = t["panel_alpha"][level]
            if t.get("shadow") and level != "group" and not dashed:
                s.rect(x, y, w, h, r, fill=t["surface"], filt="shadow")
            s.rect(x, y, w, h, r, fill=t["surface"] if level != "group" else "none")
            s.rect(x, y, w, h, r, fill=c, fill_opacity=a[0], stroke=c, sw=sw,
                   stroke_opacity=t["panel_stroke_alpha"][level], dash=dash)
        if t.get("accent_bar") and level == "band" and not dashed:
            s.rect(x, y + 10, 4, h - 20, 2, fill=c)

    def chip(self, x, y, w, h, key, title, sub=None, big=False, dashed=False, muted=False):
        s, t = self.s, self.t
        c = self.col(key)
        r = max(self.r - 3, 3)
        style = t["chip"]
        if s.dark:
            if style == "glass":
                fill = s.vgrad([(0, mix(c, "#ffffff", 0.15), t["chip_alpha"][0]), (1, c, t["chip_alpha"][1])])
                s.rect(x, y, w, h, r, fill=t["surface"])
                s.rect(x, y, w, h, r, fill=fill, stroke=c, sw=t["stroke"], stroke_opacity=0.9, dash="5 4" if dashed else None)
                if t.get("highlight") and not dashed:
                    s.rect(x + r, y + 1, w - 2 * r, 1, 0.5, fill="#ffffff", fill_opacity=t["highlight"] * 1.6)
                tc = t["text"]
            elif style == "tint":
                s.rect(x, y, w, h, r, fill=t["surface"])
                s.rect(x, y, w, h, r, fill=c, fill_opacity=t["chip_alpha"][0], stroke=c, sw=t["stroke"],
                       stroke_opacity=t["chip_alpha"][1], dash="5 4" if dashed else None)
                tc = mix(c, "#ffffff", 0.78)
            elif style == "outline":
                s.rect(x, y, w, h, r, fill=t["surface"], stroke=c, sw=t["stroke"], dash="5 4" if dashed else None)
                tc = t["text"]
            else:  # solid
                fill = s.vgrad([(0, mix(c, "#ffffff", 0.12), 1), (1, mix(c, "#000000", 0.25), 1)])
                s.rect(x, y, w, h, r, fill=fill, stroke=mix(c, "#ffffff", 0.35), sw=t["stroke"], dash="5 4" if dashed else None)
                tc = "#ffffff"
            sc = mix(c, "#ffffff", 0.55) if style != "solid" else mix(c, "#ffffff", 0.8)
        else:
            if style == "solid" and not muted:
                s.rect(x, y, w, h, r, fill=c, stroke=mix(c, "#000000", 0.2), sw=t["stroke"], dash="5 4" if dashed else None)
                tc, sc = "#ffffff", mix(c, "#ffffff", 0.78)
            else:
                s.rect(x, y, w, h, r, fill="#ffffff", stroke=c, sw=t["stroke"],
                       stroke_opacity=0.75, dash="5 4" if dashed else None)
                if style == "tint":
                    s.rect(x, y, w, h, r, fill=c, fill_opacity=0.07)
                tc, sc = t["text"], mix(c, "#000000", 0.3)
        if muted:
            tc = sc = t["muted"]
        self.block(x + w / 2, y + h / 2, w - 14, title, sub, tc, sc, big)

    def block_h(self, w, title, sub, big=False):
        fs = self.fs * (1.3 if big else 1)
        tl, tfs = wrap(title, fs, w)
        h = len(tl) * tfs * self.lh
        if sub:
            sl, sfs = wrap(sub, self.sub_fs * (1.15 if big else 1), w, bold=False)
            h += len(sl) * sfs * self.lh + 3
        return h

    def block(self, cx, cy, w, title, sub, tc, sc, big=False):
        fs = self.fs * (1.3 if big else 1)
        tl, tfs = wrap(title, fs, w)
        rows = [(l, tfs, True, tc) for l in tl]
        if sub:
            sl, sfs = wrap(sub, self.sub_fs * (1.15 if big else 1), w, bold=False)
            rows += [(l, sfs, False, sc) for l in sl]
        total = sum(r[1] * self.lh for r in rows) + (3 if sub else 0)
        yy = cy - total / 2
        for i, (l, f, b, c) in enumerate(rows):
            if not b and i and rows[i - 1][2]:
                yy += 3
            yy += f * self.lh
            self.s.text(cx, yy - f * self.lh * 0.5 + f * 0.36, l, f, c, bold=b)

    # -- labels ---------------------------------------------------------------
    def rail_label(self, x, y, h, text, key, w):
        lines = text.split("\n")
        f = self.t["label_font"]
        c = self.label_color(key)
        lines_f = []
        for l in lines:
            ff = f
            while ff > 9 and text_w(l, ff, mono=self.mono_labels) > w - 6:
                ff -= 0.5
            lines_f.append((l, ff))
        total = sum(ff * 1.2 for _, ff in lines_f)
        yy = y + h / 2 - total / 2
        for l, ff in lines_f:
            yy += ff * 1.2
            self.s.text(x, yy - ff * 0.25, l, ff, c, anchor="start", mono=self.mono_labels)

    def eyebrow(self, x, y, text, key):
        f = self.t["eyebrow_font"]
        txt = text.replace("\n", " ").upper()
        self.s.text(x, y + f, txt, f, self.label_color(key), anchor="start",
                    mono=self.mono_labels, spacing=1.4)

    # -- composite ------------------------------------------------------------
    def chip_widths(self, w, items, n_gap):
        n = len(items)
        g = n_gap
        eq = (w - g * (n - 1)) / n
        nat = [max(text_w(t, self.fs), text_w(s or "", self.sub_fs, False)) + 26 for t, s in items]
        if max(nat) <= eq or not self.t.get("flex", True):
            return [eq] * n
        tot = sum(nat)
        avail = w - g * (n - 1)
        return [avail * v / tot for v in nat]

    def row_h(self, widths, items):
        return max(self.block_h(cw - 14, t, s) for cw, (t, s) in zip(widths, items)) + 2 * self.t["chip_vpad"]

    def band(self, x, y, w, key, label, items, rail_w=None, min_h=0, h=None, labels=None):
        """A layer band with a label and a row of chips. Return its height."""
        t = self.t
        rail = (labels or t["labels"]) == "rail"
        rail_w = rail_w or t["rail_w"]
        p = self.pad
        cg = t["chip_gap"]
        if rail:
            ix, iw = x + rail_w, w - rail_w - p
            top = p
        else:
            ix, iw = x + p, w - 2 * p
            top = p + t["eyebrow_font"] + 10
        widths = self.chip_widths(iw, items, cg)
        ch = max(self.row_h(widths, items), t["chip_min_h"])
        bh = h or max(top + ch + p, min_h)
        ch = bh - top - p
        self.panel(x, y, w, bh, key)
        if rail:
            self.rail_label(x + p + (6 if t.get("accent_bar") else 0), y, bh, label, key, rail_w - p)
        else:
            self.eyebrow(x + p, y + p - 2, label, key)
        cx = ix
        for cw, (ti, su) in zip(widths, items):
            self.chip(cx, y + top, cw, ch, key, ti, su)
            cx += cw + cg
        return bh

    def band_h(self, w, items, rail_w=None, labels=None):
        t = self.t
        rail = (labels or t["labels"]) == "rail"
        rail_w = rail_w or t["rail_w"]
        iw = w - rail_w - self.pad if rail else w - 2 * self.pad
        top = self.pad if rail else self.pad + t["eyebrow_font"] + 10
        widths = self.chip_widths(iw, items, t["chip_gap"])
        return top + max(self.row_h(widths, items), t["chip_min_h"]) + self.pad


# --------------------------------------------------------------------------
# The diagram
# --------------------------------------------------------------------------


def build(t):
    L = Lay(t)
    s = L.s
    W = t["width"]
    M = t["margin"]
    g = L.g
    p = L.pad
    cw = W - 2 * M
    rail = t["labels"] == "rail"

    y = M
    # ---- Applications ----------------------------------------------------
    if t.get("header"):
        s.text(M, y + 22, "Media Transport Library", 24, t["text"], anchor="start")
        s.text(M, y + 44, "Architecture overview", 13, t["muted"], bold=False, anchor="start")
        y += 64
    h = L.band(M, y, cw, "apps", "Applications", APPS)
    apps_bottom = y + h
    y += h

    # ---- API connectors ----------------------------------------------------
    conn_h = t["conn_h"]
    mtl_y = y + conn_h
    # ---- MTL frame -------------------------------------------------------
    ip = p + 4
    mx, mw = M + t["mtl_inset"], cw - 2 * t["mtl_inset"]
    title_h = t["title_size"] + 30
    ix, iw = mx + ip, mw - 2 * ip

    left_w = round(iw * t["left_frac"])
    right_w = iw - left_w - g
    # row 1: pipeline + color conversion
    pipe_w = round(left_w * 0.60)
    col_w = left_w - pipe_w - g
    pipe_h = L.band_h(pipe_w, PIPELINE, t["rail_w_small"])
    st_h = L.band_h(left_w, ST2110, t["rail_w_small"])
    ctrl_need = t["eyebrow_font"] + 10 + 3 * 40 + 2 * t["chip_gap"] + 2 * p + 4
    if rail:
        ctrl_need += 6
    left_h = pipe_h + g + st_h
    if ctrl_need > left_h:
        st_h += ctrl_need - left_h
        left_h = ctrl_need
    mg_h = L.band_h(iw, MGMT)
    dal_h = L.band_h(iw, DAL)
    mtl_h = title_h + left_h + g + mg_h + g + dal_h + ip
    L.panel(mx, mtl_y, mw, mtl_h, "mtl", level="outer")
    tcol = L.label_color("mtl") if t.get("title_tinted", True) else t["text"]
    if t.get("title_align", "center") == "center":
        s.text(mx + mw / 2, mtl_y + 16 + t["title_size"], "Media Transport Library", t["title_size"], tcol)
    else:
        s.text(ix + 2, mtl_y + 16 + t["title_size"], "Media Transport Library", t["title_size"], tcol, anchor="start")

    # connectors (drawn after the frame so the arrows sit on top)
    api_xs = [mx + mw * f for f in t["api_pos"]]
    ac = L.col("api")
    for ax, name in zip(api_xs, APIS):
        y1, y2 = apps_bottom + 3, mtl_y - 3
        if t["glow"]:
            s.line(ax, y1, ax, y2, ac, 6, start=False, end=False, opacity=0.5, filt="glowS")
        s.line(ax, y1, ax, y2, ac, 2.2, start=True, end=True)
        pw = text_w(name, 12.5) + 28
        ph = 24
        py = (y1 + y2) / 2 - ph / 2
        s.rect(ax - pw / 2, py, pw, ph, ph / 2, fill=t["bg_solid"])
        s.rect(ax - pw / 2, py, pw, ph, ph / 2, fill=ac, fill_opacity=0.22 if s.dark else 0.12,
               stroke=ac, sw=1.3)
        s.text(ax, py + ph / 2 + 4.5, name, 12.5, t["text"] if s.dark else mix(ac, "#000000", 0.35))

    yy = mtl_y + title_h
    L.band(ix, yy, pipe_w, "pipe", "Pipeline\nAPI", PIPELINE, t["rail_w_small"], h=pipe_h)
    # color conversion
    cx0 = ix + pipe_w + g
    L.panel(cx0, yy, col_w, pipe_h, "color")
    L.chip(cx0 + p, yy + p, col_w - 2 * p, pipe_h - 2 * p, "color", *COLOR_CONV)
    L.band(ix, yy + pipe_h + g, left_w, "st", "ST 2110\nSessions", ST2110, t["rail_w_small"], h=st_h)

    # control plane
    rx = ix + left_w + g
    L.panel(rx, yy, right_w, left_h, "ctrl")
    L.eyebrow(rx + p, yy + p - 2, "Control Plane Protocols", "ctrl")
    gy = yy + p + t["eyebrow_font"] + 10
    gh = left_h - (gy - yy) - p
    cg = t["chip_gap"]
    bw = (right_w - 2 * p - cg) / 2
    bh = (gh - 2 * cg) / 3
    pos = [(0, 0), (1, 0), (0, 1), (1, 1)]
    for (c, r_), name in zip(pos, CTRL[:4]):
        L.chip(rx + p + c * (bw + cg), gy + r_ * (bh + cg), bw, bh, "ctrl", name)
    L.chip(rx + p, gy + 2 * (bh + cg), right_w - 2 * p, bh, "ctrl", CTRL[4])

    yy += left_h + g
    L.band(ix, yy, iw, "mgmt", "Management", MGMT)
    yy += mg_h + g
    L.band(ix, yy, iw, "dal", "Device\nAbstraction\nLayer" if rail else "Device Abstraction Layer", DAL)

    y = mtl_y + mtl_h + t["gap_major"]

    # ---- Network backend ---------------------------------------------------
    nb_top = p if rail else p + t["eyebrow_font"] + 10
    inner_h = t["backend_h"]
    nb_h = nb_top + inner_h + p
    L.panel(M, y, cw, nb_h, "backend")
    if rail:
        L.rail_label(M + p + (6 if t.get("accent_bar") else 0), y, nb_h, "Network\nBackend", "backend", t["rail_w"] - p)
        bx, bw_all = M + t["rail_w"], cw - t["rail_w"] - p
    else:
        L.eyebrow(M + p, y + p - 2, "Network Backend", "backend")
        bx, bw_all = M + p, cw - 2 * p
    by = y + nb_top
    dp_w = round(bw_all * 0.22)
    win_w = round(bw_all * 0.17)
    lk_w = bw_all - dp_w - win_w - 2 * g
    L.chip(bx, by, dp_w, inner_h, "dpdk", "DPDK PMD", "Linux & Windows", big=True)
    kx = bx + dp_w + g
    L.panel(kx, by, lk_w, inner_h, "kernel", level="group")
    L.eyebrow(kx + p - 2, by + 9, "Linux Kernel", "kernel")
    ktop = by + 9 + t["eyebrow_font"] + 10
    kg = 22
    kh = (inner_h - (ktop - by) - p + 2 - kg) / 2
    arrow_gap = 64
    kcw = (lk_w - 2 * (p - 2) - arrow_gap) / 2
    k1x = kx + p - 2
    k2x = k1x + kcw + arrow_gap
    L.chip(k1x, ktop, kcw, kh, "kstack", "Kernel Network Stack")
    L.chip(k1x, ktop + kh + kg, kcw, kh, "nicdrv", "NIC Driver")
    L.chip(k2x, ktop, kcw, kh, "xdp", "AF_XDP")
    L.chip(k2x, ktop + kh + kg, kcw, kh, "ebpf", "eBPF program")
    arc = L.col("arrow") if "arrow" in t["c"] else L.col("xdp")
    ay = ktop + kh + kg + kh / 2
    s.line(k1x + kcw + 4, ay, k2x - 4, ay, arc, 2.4)
    s.line(k2x + kcw / 2, ktop + kh + kg - 2, k2x + kcw / 2, ktop + kh + 2, arc, 2.4)
    wx = kx + lk_w + g
    L.chip(wx, by, win_w, inner_h, "win", "Windows Kernel", "Not supported", dashed=True, muted=True)
    y += nb_h + t["gap_os"]

    # ---- Platforms -------------------------------------------------------
    os_h = 34
    oc = L.col("os")
    px = M + (t["rail_w"] if rail else p)
    if t.get("os_band", True):
        L.panel(M, y, cw, os_h + 12, "os")
        if rail:
            L.rail_label(M + p + (6 if t.get("accent_bar") else 0), y, os_h + 12, "Platforms", "os", t["rail_w"] - p)
        else:
            px = M + p + text_w("PLATFORMS", t["eyebrow_font"], spacing=1.4) + 24
            L.eyebrow(M + p, y + (os_h + 12) / 2 - t["eyebrow_font"] / 2 - 2, "Platforms", "os")
        oy = y + 6
    else:
        oy = y
    for name, what in PLATFORMS:
        w1 = text_w(name, 13.5) + text_w(what, 13, False) + 54
        s.rect(px, oy, w1, os_h, os_h / 2, fill=t["surface"] if s.dark else "#ffffff",
               stroke=oc, sw=1.2, stroke_opacity=0.8)
        s.rect(px + 12, oy + os_h / 2 - 4, 8, 8, 4, fill=oc)
        s.text(px + 28, oy + os_h / 2 + 4.6, name, 13.5, t["text"], anchor="start")
        s.text(px + 34 + text_w(name, 13.5), oy + os_h / 2 + 4.6, what, 13, t["muted"], bold=False, anchor="start")
        px += w1 + 14
    y += (os_h + 12 if t.get("os_band", True) else os_h) + t["gap_major"]

    # ---- Hardware --------------------------------------------------------
    nic_w = round(cw * 0.70)
    dma_w = cw - nic_w - g
    hh = max(L.band_h(nic_w, NIC_HW, labels="eyebrow"), L.band_h(dma_w, DMA_HW, labels="eyebrow"))
    L.band(M, y, nic_w, "nic", "NIC Hardware", NIC_HW, h=hh, labels="eyebrow")
    L.band(M + nic_w + g, y, dma_w, "dma", "DMA Hardware", DMA_HW, h=hh, labels="eyebrow")
    y += hh + M

    H = round(y)
    # ---- background (inserted first) ---------------------------------------
    bg = []
    if t.get("bg_grad"):
        bid, _ = s._id("bg", "main")
        stops = "".join('<stop offset="%s" stop-color="%s"/>' % (o, c) for o, c in t["bg_grad"])
        s.defs.append('<linearGradient id="%s" x1="0" y1="0" x2="%s" y2="1">%s</linearGradient>'
                      % (bid, t.get("bg_dir", 0), stops))
        bg.append('<rect width="%d" height="%d" fill="url(#%s)"/>' % (W, H, bid))
    else:
        bg.append('<rect width="%d" height="%d" fill="%s"/>' % (W, H, t["bg_solid"]))
    for i, (fx, fy, fr, c, a) in enumerate(t.get("bg_glows", [])):
        rid = "rg%d" % i
        s.defs.append('<radialGradient id="%s"><stop offset="0" stop-color="%s" stop-opacity="%.2f"/>'
                      '<stop offset="1" stop-color="%s" stop-opacity="0"/></radialGradient>' % (rid, c, a, c))
        bg.append('<circle cx="%.0f" cy="%.0f" r="%.0f" fill="url(#%s)"/>' % (fx * W, fy * H, fr * W, rid))
    pat = t.get("pattern")
    if pat:
        kind, step, c, a = pat
        if kind == "grid":
            shape = '<path d="M %d 0 L 0 0 0 %d" fill="none" stroke="%s" stroke-width="1"/>' % (step, step, c)
        else:
            shape = '<circle cx="%d" cy="%d" r="1.1" fill="%s"/>' % (step // 2, step // 2, c)
        s.defs.append('<pattern id="pat" width="%d" height="%d" patternUnits="userSpaceOnUse">%s</pattern>'
                      % (step, step, shape))
        bg.append('<rect width="%d" height="%d" fill="url(#pat)" opacity="%.2f"/>' % (W, H, a))
    s.body = bg + s.body
    return s.out(W, H)


# --------------------------------------------------------------------------
# Themes
# --------------------------------------------------------------------------

BASE = dict(
    width=1280, margin=32, gap=14, gap_major=18, gap_os=12, pad=14, radius=12,
    chip_gap=10, chip_font=14, chip_vpad=10, chip_min_h=44, label_font=15,
    eyebrow_font=11.5, rail_w=150, rail_w_small=112, labels="rail",
    left_frac=0.72, conn_h=58, mtl_inset=0, title_size=24, backend_h=128,
    api_pos=(0.16, 0.42, 0.68), stroke=1.4, chip="glass", glow=0.0,
    panel_alpha=dict(outer=(0.16, 0.06), band=(0.20, 0.08), group=(0.16, 0.06)),
    panel_stroke_alpha=dict(outer=0.9, band=0.65, group=0.55),
    chip_alpha=(0.30, 0.10), highlight=0.10, dark=True, flex=True,
)


def theme(**kw):
    t = dict(BASE)
    t.update(kw)
    return t


NEON = dict(
    apps="#2ee6ff", api="#7df9ff", mtl="#38bdf8", pipe="#ff8a3d", color="#ffb547",
    st="#ff6b3d", ctrl="#c084fc", mgmt="#a78bfa", dal="#34d399", backend="#22c55e",
    dpdk="#a78bfa", kernel="#38bdf8", kstack="#f472b6", nicdrv="#34d399", xdp="#ff8a3d",
    ebpf="#fbbf24", win="#64748b", os="#94a3b8", nic="#4ade80", dma="#38bdf8", arrow="#ff8a3d",
)

THEMES = {
    "01-neon-glass-refined": theme(
        bg_solid="#050a14", bg_grad=[(0, "#071226"), (1, "#03060d")],
        bg_glows=[(0.12, 0.1, 0.35, "#1d4ed8", 0.30), (0.9, 0.45, 0.35, "#7c3aed", 0.22),
                  (0.3, 0.95, 0.3, "#059669", 0.18)],
        surface="#0a1424", text="#eef6ff", muted="#8ea3bd", c=NEON, glow=4.0, glow_alpha=0.45,
    ),
    "02-neon-cyber-eyebrow": theme(
        labels="eyebrow", bg_solid="#04040a", bg_grad=[(0, "#0a0614"), (1, "#020206")],
        bg_glows=[(0.85, 0.08, 0.35, "#ff2bd6", 0.22), (0.1, 0.6, 0.35, "#00e5ff", 0.16)],
        pattern=("grid", 32, "#2b2f55", 0.35), surface="#0b0a18", text="#f5f3ff", muted="#9a97c2",
        c=dict(NEON, apps="#00e5ff", api="#00e5ff", mtl="#ff2bd6", pipe="#ff9e00", color="#ffd000",
               st="#ff5e3a", ctrl="#b26bff", mgmt="#7a5cff", dal="#00f5a0", backend="#00e5ff",
               dpdk="#b26bff", kernel="#00e5ff", nic="#00f5a0", dma="#00e5ff"),
        glow=5.0, glow_alpha=0.6, chip="outline", radius=10, title_size=26,
    ),
    "03-aurora-frost": theme(
        labels="eyebrow", gap=18, gap_major=22, pad=18, radius=18, chip_gap=12, chip_vpad=12,
        bg_solid="#0b1020", bg_grad=[(0, "#0b1530"), (0.55, "#101032"), (1, "#0a1a24")], bg_dir=1,
        bg_glows=[(0.2, 0.0, 0.5, "#22d3ee", 0.22), (0.85, 0.35, 0.45, "#a855f7", 0.20),
                  (0.15, 0.8, 0.45, "#10b981", 0.16)],
        surface="#0e1530", text="#f1f5ff", muted="#9fb0d0",
        c=dict(NEON, mtl="#a5b4fc", apps="#67e8f9", api="#67e8f9", pipe="#fdba74", color="#fde68a",
               st="#fb923c", ctrl="#f0abfc", mgmt="#c4b5fd", dal="#6ee7b7", backend="#5eead4"),
        panel_alpha=dict(outer=(0.12, 0.05), band=(0.14, 0.05), group=(0.12, 0.04)),
        chip_alpha=(0.22, 0.08), highlight=0.18, glow=0.0, width=1320,
    ),
    "04-midnight-flat-accent": theme(
        gap=12, gap_major=16, pad=12, radius=8, chip_gap=8, chip_vpad=9,
        bg_solid="#0b0f17", surface="#111827", text="#e5e7eb", muted="#9ca3af",
        c=dict(NEON, mtl="#60a5fa", apps="#22d3ee", pipe="#fb923c", st="#f97316", color="#facc15",
               ctrl="#c084fc", mgmt="#818cf8", dal="#34d399", backend="#4ade80"),
        chip="tint", chip_alpha=(0.16, 0.35), accent_bar=True, highlight=0.0,
        panel_alpha=dict(outer=(0.07, 0.07), band=(0.07, 0.07), group=(0.06, 0.06)),
        panel_stroke_alpha=dict(outer=0.55, band=0.22, group=0.3), rail_w=158,
    ),
    "05-github-dark": theme(
        labels="eyebrow", radius=8, bg_solid="#0d1117", surface="#161b22", text="#e6edf3",
        muted="#8d96a0",
        c=dict(apps="#58a6ff", api="#79c0ff", mtl="#58a6ff", pipe="#f0883e", color="#d29922",
               st="#f78166", ctrl="#bc8cff", mgmt="#a371f7", dal="#3fb950", backend="#3fb950",
               dpdk="#a371f7", kernel="#58a6ff", kstack="#db61a2", nicdrv="#3fb950", xdp="#f0883e",
               ebpf="#d29922", win="#6e7681", os="#8d96a0", nic="#3fb950", dma="#58a6ff", arrow="#f0883e"),
        chip="outline", stroke=1.2, highlight=0.0,
        panel_alpha=dict(outer=(0.05, 0.05), band=(0.06, 0.06), group=(0.05, 0.05)),
        panel_stroke_alpha=dict(outer=0.6, band=0.35, group=0.35), title_align="left",
    ),
    "06-intel-blue-glow": theme(
        bg_solid="#00152b", bg_grad=[(0, "#002244"), (1, "#000b18")],
        bg_glows=[(0.5, 0.0, 0.55, "#0068b5", 0.35), (0.5, 1.0, 0.4, "#00c7fd", 0.10)],
        surface="#03213d", text="#f2f8ff", muted="#8fb3d6",
        c=dict(apps="#00c7fd", api="#00c7fd", mtl="#00c7fd", pipe="#ffa300", color="#fec91b",
               st="#ff7a1a", ctrl="#c18cff", mgmt="#8f9bff", dal="#00d6a4", backend="#2fbf71",
               dpdk="#8f6bff", kernel="#0098ff", kstack="#e96115", nicdrv="#2fbf71", xdp="#ff7a1a",
               ebpf="#fec91b", win="#5b7a99", os="#7fb2e5", nic="#2fbf71", dma="#00c7fd", arrow="#ffa300"),
        glow=3.0, glow_alpha=0.4, chip="glass", radius=10,
    ),
    "07-blueprint-mono": theme(
        labels="eyebrow", mono_labels=True, radius=4, stroke=1.2,
        bg_solid="#071a33", bg_grad=[(0, "#0a2342"), (1, "#06162b")],
        pattern=("grid", 24, "#2a5b8f", 0.35), surface="#0a2240", text="#e3f2ff", muted="#8bb4dc",
        c={k: "#7cc7ff" for k in NEON} | dict(st="#ffd27c", pipe="#ffd27c", color="#ffd27c",
                                              ctrl="#a6f0ff", win="#4f7aa6", xdp="#ffd27c",
                                              ebpf="#ffd27c", arrow="#ffd27c"),
        chip="outline", highlight=0.0,
        panel_alpha=dict(outer=(0.05, 0.05), band=(0.06, 0.06), group=(0.04, 0.04)),
        panel_stroke_alpha=dict(outer=0.9, band=0.55, group=0.5),
    ),
    "08-synthwave": theme(
        bg_solid="#14072b", bg_grad=[(0, "#1a0838"), (0.6, "#2a0a3d"), (1, "#0e0420")],
        bg_glows=[(0.5, 1.05, 0.55, "#ff2e88", 0.25), (0.1, 0.05, 0.3, "#00e1ff", 0.18)],
        surface="#1b0b33", text="#fff1fb", muted="#c6a6d8",
        c=dict(apps="#00e1ff", api="#00e1ff", mtl="#ff4fd8", pipe="#ff9f43", color="#ffd166",
               st="#ff6b6b", ctrl="#b388ff", mgmt="#8c7bff", dal="#2de2c4", backend="#2de2c4",
               dpdk="#b388ff", kernel="#00e1ff", kstack="#ff4fd8", nicdrv="#2de2c4", xdp="#ff9f43",
               ebpf="#ffd166", win="#7a6a90", os="#c6a6d8", nic="#2de2c4", dma="#00e1ff", arrow="#ffd166"),
        glow=4.5, glow_alpha=0.55, radius=14, label_lift=0.25,
    ),
    "09-emerald-terminal": theme(
        labels="eyebrow", mono_labels=True, radius=6, gap=12, pad=12, chip_gap=8,
        bg_solid="#030806", bg_grad=[(0, "#04110c"), (1, "#020504")],
        pattern=("dots", 18, "#1f5c45", 0.55), surface="#06140f", text="#e6fff4", muted="#7fb59e",
        c=dict(apps="#34f5c5", api="#34f5c5", mtl="#22c55e", pipe="#facc15", color="#fde047",
               st="#a3e635", ctrl="#2dd4bf", mgmt="#5eead4", dal="#4ade80", backend="#10b981",
               dpdk="#2dd4bf", kernel="#34d399", kstack="#a3e635", nicdrv="#4ade80", xdp="#facc15",
               ebpf="#fde047", win="#3f5f52", os="#7fb59e", nic="#4ade80", dma="#2dd4bf", arrow="#facc15"),
        glow=3.0, glow_alpha=0.4, chip="tint", chip_alpha=(0.14, 0.6), highlight=0.0,
    ),
    "10-graphite-focus": theme(
        bg_solid="#111214", surface="#1a1c1f", text="#f3f4f6", muted="#9ca3af",
        c={k: "#6b7280" for k in NEON} | dict(mtl="#22d3ee", st="#22d3ee", pipe="#22d3ee",
                                              color="#22d3ee", ctrl="#22d3ee", mgmt="#22d3ee",
                                              dal="#22d3ee", api="#22d3ee", xdp="#f59e0b",
                                              arrow="#f59e0b", ebpf="#f59e0b", win="#4b5563"),
        chip="outline", label_lift=0.45, highlight=0.0, glow=2.5, glow_alpha=0.3,
        panel_alpha=dict(outer=(0.08, 0.03), band=(0.08, 0.04), group=(0.06, 0.03)),
        panel_stroke_alpha=dict(outer=0.9, band=0.4, group=0.4), radius=10,
    ),
    "11-high-contrast": theme(
        chip_font=15.5, label_font=16, eyebrow_font=13, stroke=2.2, radius=8, width=1340,
        bg_solid="#000000", surface="#000000", text="#ffffff", muted="#d1d5db",
        c=dict(apps="#00d7ff", api="#00d7ff", mtl="#ffffff", pipe="#ff9a00", color="#ffd400",
               st="#ff9a00", ctrl="#d08cff", mgmt="#b9a6ff", dal="#3dff8f", backend="#3dff8f",
               dpdk="#d08cff", kernel="#00d7ff", kstack="#ff6fd8", nicdrv="#3dff8f", xdp="#ff9a00",
               ebpf="#ffd400", win="#9ca3af", os="#ffffff", nic="#3dff8f", dma="#00d7ff", arrow="#ffd400"),
        chip="outline", highlight=0.0, label_lift=0.1,
        panel_alpha=dict(outer=(0.0, 0.0), band=(0.10, 0.10), group=(0.08, 0.08)),
        panel_stroke_alpha=dict(outer=1.0, band=0.9, group=0.8), backend_h=136,
    ),
    "12-light-frost": theme(
        dark=False, labels="eyebrow", radius=14, gap=16, pad=16, chip_gap=10,
        bg_solid="#eef3f9", bg_grad=[(0, "#f4f8fc"), (1, "#e6edf6")], surface="#ffffff",
        text="#0f1b2d", muted="#5b6b80",
        c=dict(apps="#0891b2", api="#0e7490", mtl="#2563eb", pipe="#ea580c", color="#ca8a04",
               st="#ea580c", ctrl="#9333ea", mgmt="#7c3aed", dal="#059669", backend="#16a34a",
               dpdk="#7c3aed", kernel="#0284c7", kstack="#db2777", nicdrv="#16a34a", xdp="#ea580c",
               ebpf="#ca8a04", win="#94a3b8", os="#475569", nic="#16a34a", dma="#0284c7", arrow="#ea580c"),
        chip="tint", shadow=(4, 8, "#1e3a5f", 0.12),
        panel_alpha=dict(outer=(0.03, 0), band=(0.06, 0), group=(0.05, 0)),
        panel_stroke_alpha=dict(outer=0.55, band=0.35, group=0.4),
    ),
    "13-light-intel-clean": theme(
        dark=False, radius=8, bg_solid="#ffffff", surface="#ffffff", text="#1b2533", muted="#5a6778",
        c=dict(apps="#0079b8", api="#0068b5", mtl="#0068b5", pipe="#d4560f", color="#a87b0a",
               st="#d4560f", ctrl="#8f5da2", mgmt="#5b4fc4", dal="#2c9c62", backend="#3b8a2f",
               dpdk="#6d4ca6", kernel="#0071c5", kstack="#a6452c", nicdrv="#3b8a2f", xdp="#d4560f",
               ebpf="#a87b0a", win="#9aa6b2", os="#525e6b", nic="#3b8a2f", dma="#0071c5", arrow="#d4560f"),
        chip="solid", stroke=1.0, accent_bar=False, title_tinted=True,
        panel_alpha=dict(outer=(0.04, 0), band=(0.08, 0), group=(0.07, 0)),
        panel_stroke_alpha=dict(outer=0.9, band=0.45, group=0.5), shadow=(2, 4, "#000000", 0.10),
    ),
    "14-copper-night": theme(
        labels="eyebrow", radius=12, gap=16, pad=16,
        bg_solid="#0f0d0b", bg_grad=[(0, "#17120e"), (1, "#0a0908")],
        bg_glows=[(0.85, 0.15, 0.4, "#c2410c", 0.18), (0.1, 0.85, 0.4, "#0f766e", 0.18)],
        surface="#17130f", text="#fbf3ea", muted="#b7a796",
        c=dict(apps="#5eead4", api="#5eead4", mtl="#fdba74", pipe="#fb923c", color="#fcd34d",
               st="#f97316", ctrl="#f0abfc", mgmt="#c4b5fd", dal="#5eead4", backend="#2dd4bf",
               dpdk="#c4b5fd", kernel="#67e8f9", kstack="#fda4af", nicdrv="#5eead4", xdp="#fb923c",
               ebpf="#fcd34d", win="#6b5f55", os="#b7a796", nic="#5eead4", dma="#67e8f9", arrow="#fcd34d"),
        glow=3.0, glow_alpha=0.35, chip="glass", chip_alpha=(0.22, 0.06),
    ),
}

if __name__ == "__main__":
    for name, t in THEMES.items():
        path = os.path.join(OUT_DIR, "arch-%s.svg" % name)
        with open(path, "w") as f:
            f.write(build(t))
        print(path)
