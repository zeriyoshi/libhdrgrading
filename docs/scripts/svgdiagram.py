# SPDX-License-Identifier: Apache-2.0 OR MIT
# Copyright (c) 2026 Go Kudo <zeriyoshi@gmail.com>
"""A small renderer of box-and-arrow diagrams to standalone SVG.

A diagram is data: boxes and groups at explicit coordinates (in SVG user
units), arrows between them, free text. Labels are either one string for
every language or a dict {"en": ..., "ja": ...}; "\n" breaks a line. The
renderer estimates the width of the text and reports labels that do not fit
their box, so a layout edit that breaks one shows up when the manual is
built (build.py fails on warnings with --strict).
"""

from __future__ import annotations

import html
from dataclasses import dataclass, field

FONT_SANS = ('system-ui, -apple-system, "Segoe UI", "Hiragino Sans", '
             '"Noto Sans CJK JP", "Noto Sans JP", "Yu Gothic UI", sans-serif')
FONT_MONO = ('ui-monospace, "SF Mono", Menlo, Consolas, "Noto Sans Mono", '
             '"DejaVu Sans Mono", monospace')

# Box kinds: (light fill, light stroke, dark fill, dark stroke)
KINDS = {
    'app': ('#eef0ff', '#4c56c8', '#20233f', '#8f97ef'),
    'api': ('#e5f2fd', '#1c6fb8', '#13283b', '#6cb2ee'),
    'entry': ('#e3f8f3', '#0b7f87', '#11302f', '#5ccdc4'),
    'core': ('#e8f7ea', '#2f8540', '#16301b', '#77c987'),
    'io': ('#fdf0e2', '#c2550f', '#3a2512', '#f0a061'),
    'plugin': ('#f6ecfb', '#8f35ab', '#2f1a38', '#cc8ae2'),
    'filter': ('#fbeaf3', '#b0306a', '#371827', '#e483b0'),
    'gpu': ('#fdebeb', '#bf3434', '#3a1818', '#ef8a8a'),
    'common': ('#f0f1f3', '#505865', '#24272c', '#a3aab6'),
    'ext': ('#fdf6d8', '#a86a00', '#332b10', '#e2b84f'),
    'data': ('#ffffff', '#7d858f', '#1b1d21', '#8d949e'),
}

STYLE_TEMPLATE = """
.hd-svg {{ {light} }}
@media (prefers-color-scheme: dark) {{
  .hd-svg {{ {dark} }}
  html[data-theme="light"] .hd-svg {{ {light} }}
}}
html[data-theme="dark"] .hd-svg {{ {dark} }}
.hd-svg .hd-bg {{ fill: var(--hd-bg); }}
.hd-svg text {{ font-family: {sans}; fill: var(--hd-ink); }}
.hd-svg .hd-mono {{ font-family: {mono}; }}
.hd-svg .hd-label {{ font-size: 13px; font-weight: 600; }}
.hd-svg .hd-sub {{ font-size: 11px; fill: var(--hd-muted); }}
.hd-svg .hd-gtitle {{ font-size: 12px; font-weight: 700; fill: var(--hd-muted); }}
.hd-svg .hd-note {{ font-size: 11.5px; fill: var(--hd-muted); }}
.hd-svg .hd-elabel {{ font-size: 11px; fill: var(--hd-muted); }}
.hd-svg .hd-elabel-bg {{ fill: var(--hd-bg); opacity: .92; }}
.hd-svg .hd-group {{ fill: var(--hd-group); stroke: var(--hd-group-stroke); stroke-width: 1; }}
.hd-svg .hd-edge {{ fill: none; stroke: var(--hd-line); stroke-width: 1.4; }}
.hd-svg .hd-edge.hd-dashed {{ stroke-dasharray: 5 4; }}
.hd-svg .hd-edge.hd-accent {{ stroke: var(--hd-accent); stroke-width: 1.8; }}
.hd-svg .hd-arrow {{ fill: var(--hd-line); }}
.hd-svg .hd-arrow.hd-accent {{ fill: var(--hd-accent); }}
{kinds}
"""

LIGHT = {'bg': '#fafaf9', 'ink': '#1c1917', 'muted': '#57534e', 'line': '#6b6560',
         'group': '#f5f5f4', 'group-stroke': '#d6d3d1', 'accent': '#b45309'}
DARK = {'bg': '#0c0a09', 'ink': '#f5f5f4', 'muted': '#a8a29e', 'line': '#a8a29e',
        'group': '#1c1917', 'group-stroke': '#44403c', 'accent': '#f59e0b'}

KIND_RULE = ('.hd-svg .hd-k-{name} rect.hd-box {{ fill: var(--hd-{name}-fill); '
             'stroke: var(--hd-{name}-stroke); stroke-width: 1.3; }}')


def _style() -> str:
    light = [f'--hd-{k}: {v};' for k, v in LIGHT.items()]
    dark = [f'--hd-{k}: {v};' for k, v in DARK.items()]
    kinds = []

    for name, (lf, ls, df, ds) in KINDS.items():
        light.append(f'--hd-{name}-fill: {lf}; --hd-{name}-stroke: {ls};')
        dark.append(f'--hd-{name}-fill: {df}; --hd-{name}-stroke: {ds};')
        kinds.append(KIND_RULE.format(name=name))

    kinds.append('.hd-svg .hd-k-ext rect.hd-box, .hd-svg .hd-k-data rect.hd-box '
                 '{ stroke-dasharray: 4 3; }')

    return STYLE_TEMPLATE.format(light=' '.join(light), dark=' '.join(dark), sans=FONT_SANS,
                                 mono=FONT_MONO, kinds='\n'.join(kinds))


def pick(label, lang: str) -> str:
    """The label in lang (a str is every language's)."""
    if label is None:
        return ''

    if isinstance(label, str):
        return label

    return label.get(lang, label.get('en', ''))


def text_width(text: str, size: float, mono: bool = False) -> float:
    """An estimate of the rendered width of one line (the sans font is narrower)."""
    width = 0.0

    for ch in text:
        code = ord(ch)
        if code >= 0x2E80:
            width += 1.0
        elif mono:
            width += 0.62
        elif ch in 'il.,:;|!\'`()[]{} /':
            width += 0.32
        elif ch in 'mwMW':
            width += 0.86
        elif ch.isupper() or ch.isdigit():
            width += 0.66
        else:
            width += 0.56

    return width * size


class Rect:
    """What edges attach to: a box or a group."""

    def center(self):
        return (self.x + self.w / 2, self.y + self.h / 2)

    def side_point(self, side: str, at: float = 0.5):
        if side == 'top':
            return (self.x + self.w * at, self.y)
        if side == 'bottom':
            return (self.x + self.w * at, self.y + self.h)
        if side == 'left':
            return (self.x, self.y + self.h * at)

        return (self.x + self.w, self.y + self.h * at)


@dataclass
class Box(Rect):
    id: str
    x: float
    y: float
    w: float
    h: float
    label: object
    sub: object = None
    kind: str = 'core'
    mono_sub: bool = True


@dataclass
class Group(Rect):
    id: str
    x: float
    y: float
    w: float
    h: float
    label: object
    sub: object = None
    plain: bool = False  # no fill: only the title (a lane)


@dataclass
class Line:
    points: list
    arrow: bool = False
    dashed: bool = False
    cls: str = 'edge'


@dataclass
class Edge:
    src: str
    dst: str
    label: object = None
    dashed: bool = False
    accent: bool = False
    via: list = field(default_factory=list)
    src_side: str = ''
    dst_side: str = ''
    sa: float = 0.5
    da: float = 0.5
    label_at: int = -1  # the segment the label sits on (-1: the longest)
    label_dx: float = 0.0
    label_dy: float = 0.0
    both: bool = False  # an arrowhead at both ends


@dataclass
class Text:
    x: float
    y: float
    label: object
    cls: str = 'note'
    anchor: str = 'start'


class Diagram:
    def __init__(self, name: str, width: float, height: float, title, desc=None):
        self.name = name
        self.width = width
        self.height = height
        self.title = title
        self.desc = desc
        self.groups: list[Group] = []
        self.boxes: dict[str, Box] = {}
        self.edges: list[Edge] = []
        self.texts: list[Text] = []
        self.lines: list[Line] = []

    def group(self, id, x, y, w, h, label, sub=None, plain=False):
        self.groups.append(Group(id, x, y, w, h, label, sub, plain))

    def line(self, points, arrow=False, dashed=False, cls='edge'):
        self.lines.append(Line(points, arrow, dashed, cls))

    def rect(self, id):
        if id in self.boxes:
            return self.boxes[id]

        for group in self.groups:
            if group.id == id:
                return group

        raise KeyError(f'{self.name}: no box or group {id}')

    def box(self, id, x, y, w, h, label, sub=None, kind='core', mono_sub=True):
        if id in self.boxes:
            raise ValueError(f'{self.name}: box {id} twice')

        self.boxes[id] = Box(id, x, y, w, h, label, sub, kind, mono_sub)

    def edge(self, src, dst, label=None, **kw):
        self.edges.append(Edge(src, dst, label, **kw))

    def text(self, x, y, label, cls='note', anchor='start'):
        self.texts.append(Text(x, y, label, cls, anchor))

    # --- checks ---

    def check(self, lang: str) -> list[str]:
        warnings = []

        for box in self.boxes.values():
            label = pick(box.label, lang).split('\n')
            sub = pick(box.sub, lang).split('\n') if box.sub else []
            widths = [(text_width(t, 13), t) for t in label] + \
                [(text_width(t, 11, box.mono_sub), t) for t in sub]
            widest, line = max(widths)
            if widest > box.w - 6:
                warnings.append(f'{self.name}/{lang}: "{line}" is {widest:.0f} wide '
                                f'in a box of {box.w:.0f}')

            lines = len(label) * 16 + len(sub) * 14
            if lines > box.h - 6:
                warnings.append(f'{self.name}/{lang}: "{label[0]}" is {lines} tall '
                                f'in a box of {box.h:.0f}')

            if box.x < 0 or box.y < 0 or box.x + box.w > self.width or \
               box.y + box.h > self.height:
                warnings.append(f'{self.name}: box {box.id} outside the diagram')

        for group in self.groups:
            title = pick(group.label, lang)
            if text_width(title, 12) > group.w - 16:
                warnings.append(f'{self.name}/{lang}: group "{title}" too narrow')

        ids = list(self.boxes.values())
        for i, a in enumerate(ids):
            for b in ids[i + 1:]:
                if a.x < b.x + b.w and b.x < a.x + a.w and a.y < b.y + b.h and b.y < a.y + a.h:
                    warnings.append(f'{self.name}: boxes {a.id} and {b.id} overlap')

        names = set(self.boxes) | {g.id for g in self.groups}
        for edge in self.edges:
            for end in (edge.src, edge.dst):
                if end not in names:
                    warnings.append(f'{self.name}: edge to unknown box {end}')

        return warnings

    # --- rendering ---

    def _sides(self, a: Rect, b, edge: Edge):
        """The sides an edge leaves a and enters toward the point or box b."""
        ax, ay = a.center()
        bx, by = b.center() if isinstance(b, Rect) else b
        dx, dy = bx - ax, by - ay

        if abs(dy) * max(a.w, 1) > abs(dx) * max(a.h, 1) * 0.6:
            return ('bottom', 'top') if dy > 0 else ('top', 'bottom')

        return ('right', 'left') if dx > 0 else ('left', 'right')

    def _path(self, edge: Edge):
        a, b = self.rect(edge.src), self.rect(edge.dst)
        first = edge.via[0] if edge.via else b
        last = edge.via[-1] if edge.via else a
        src_side = edge.src_side or self._sides(a, first, edge)[0]

        if edge.dst_side:
            dst_side = edge.dst_side
        else:
            dst_side = self._sides(b, last, edge)[0]

        points = [a.side_point(src_side, edge.sa)]
        points.extend(edge.via)
        points.append(b.side_point(dst_side, edge.da))

        return points, dst_side, src_side

    @staticmethod
    def _arrow(point, side: str, cls: str) -> str:
        x, y = point
        s = 7.0
        # the arrow points into the box through its side
        if side == 'top':
            pts = [(x, y), (x - s / 2, y - s), (x + s / 2, y - s)]
        elif side == 'bottom':
            pts = [(x, y), (x - s / 2, y + s), (x + s / 2, y + s)]
        elif side == 'left':
            pts = [(x, y), (x - s, y - s / 2), (x - s, y + s / 2)]
        else:
            pts = [(x, y), (x + s, y - s / 2), (x + s, y + s / 2)]

        coords = ' '.join(f'{px:.1f},{py:.1f}' for px, py in pts)

        return f'<polygon class="{cls}" points="{coords}"/>'

    def _edge_svg(self, edge: Edge, lang: str) -> list[str]:
        points, dst_side, src_side = self._path(edge)
        out = []
        cls = 'hd-edge' + (' hd-dashed' if edge.dashed else '') + (' hd-accent' if edge.accent else '')
        acls = 'hd-arrow' + (' hd-accent' if edge.accent else '')
        # stop the line at the arrowhead's base
        trimmed = list(points)
        tx, ty = trimmed[-1]
        offset = {'top': (0, -6), 'bottom': (0, 6), 'left': (-6, 0), 'right': (6, 0)}[dst_side]
        trimmed[-1] = (tx + offset[0], ty + offset[1])

        if edge.both:
            sx, sy = trimmed[0]
            offset = {'top': (0, -6), 'bottom': (0, 6), 'left': (-6, 0),
                      'right': (6, 0)}[src_side]
            trimmed[0] = (sx + offset[0], sy + offset[1])

        coords = ' '.join(f'{x:.1f},{y:.1f}' for x, y in trimmed)
        out.append(f'<polyline class="{cls}" points="{coords}"/>')
        out.append(self._arrow(points[-1], dst_side, acls))

        if edge.both:
            out.append(self._arrow(points[0], src_side, acls))

        label = pick(edge.label, lang)
        if label:
            segs = list(zip(points, points[1:]))
            if 0 <= edge.label_at < len(segs):
                seg = segs[edge.label_at]
            else:
                seg = max(segs, key=lambda s: abs(s[1][0] - s[0][0]) + abs(s[1][1] - s[0][1]))
            mx = (seg[0][0] + seg[1][0]) / 2 + edge.label_dx
            my = (seg[0][1] + seg[1][1]) / 2 + edge.label_dy
            lines = label.split('\n')
            width = max(text_width(t, 11) for t in lines) + 8
            height = 14 * len(lines) + 2
            out.append(f'<rect class="hd-elabel-bg" x="{mx - width / 2:.1f}" '
                       f'y="{my - height / 2:.1f}" width="{width:.1f}" height="{height:.1f}" '
                       'rx="3"/>')
            for i, t in enumerate(lines):
                ly = my - height / 2 + 12 + i * 14
                out.append(f'<text class="hd-elabel" x="{mx:.1f}" y="{ly:.1f}" '
                           f'text-anchor="middle">{html.escape(t)}</text>')

        return out

    def render(self, lang: str) -> str:
        title = pick(self.title, lang)
        desc = pick(self.desc, lang)
        out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.width} {self.height}" '
               f'width="{self.width}" height="{self.height}" class="hd-svg" role="img" '
               f'aria-labelledby="{self.name}-title" lang="{lang}">',
               f'<title id="{self.name}-title">{html.escape(title)}</title>']

        if desc:
            out.append(f'<desc>{html.escape(desc)}</desc>')

        out.append(f'<style>{_style()}</style>')
        out.append(f'<rect class="hd-bg" x="0" y="0" width="{self.width}" height="{self.height}"/>')

        for group in self.groups:
            if not group.plain:
                out.append(f'<rect class="hd-group" x="{group.x}" y="{group.y}" width="{group.w}" '
                           f'height="{group.h}" rx="10"/>')
            out.append(f'<text class="hd-gtitle" x="{group.x + 12}" y="{group.y + 18}">'
                       f'{html.escape(pick(group.label, lang))}</text>')
            if group.sub:
                out.append(f'<text class="hd-sub hd-mono" x="{group.x + group.w - 12}" '
                           f'y="{group.y + 18}" text-anchor="end">'
                           f'{html.escape(pick(group.sub, lang))}</text>')

        for line in self.lines:
            pts = list(line.points)
            cls = 'hd-' + line.cls + (' hd-dashed' if line.dashed else '')
            if line.arrow:
                (x0, y0), (x1, y1) = pts[-2], pts[-1]
                side = ('top' if y1 > y0 else 'bottom') if abs(y1 - y0) > abs(x1 - x0) else \
                    ('left' if x1 > x0 else 'right')
                off = {'top': (0, -6), 'bottom': (0, 6), 'left': (-6, 0), 'right': (6, 0)}[side]
                pts[-1] = (x1 + off[0], y1 + off[1])
            coords = ' '.join(f'{x:.1f},{y:.1f}' for x, y in pts)
            out.append(f'<polyline class="{cls}" points="{coords}"/>')
            if line.arrow:
                out.append(self._arrow(line.points[-1], side, 'hd-arrow'))

        for edge in self.edges:
            out.extend(self._edge_svg(edge, lang))

        for box in self.boxes.values():
            label = pick(box.label, lang).split('\n')
            sub = pick(box.sub, lang).split('\n') if box.sub else []
            total = len(label) * 16 + len(sub) * 14
            cx = box.x + box.w / 2
            y = box.y + (box.h - total) / 2 + 12
            out.append(f'<g class="hd-k-{box.kind}">')
            out.append(f'<rect class="hd-box" x="{box.x}" y="{box.y}" width="{box.w}" '
                       f'height="{box.h}" rx="7"/>')
            for t in label:
                out.append(f'<text class="hd-label" x="{cx:.1f}" y="{y:.1f}" '
                           f'text-anchor="middle">{html.escape(t)}</text>')
                y += 16
            for t in sub:
                mono = ' hd-mono' if box.mono_sub else ''
                out.append(f'<text class="hd-sub{mono}" x="{cx:.1f}" y="{y - 1:.1f}" '
                           f'text-anchor="middle">{html.escape(t)}</text>')
                y += 14
            out.append('</g>')

        for t in self.texts:
            for i, line in enumerate(pick(t.label, lang).split('\n')):
                out.append(f'<text class="hd-{t.cls}" x="{t.x}" y="{t.y + i * 15}" '
                           f'text-anchor="{t.anchor}">{html.escape(line)}</text>')

        out.append('</svg>')

        return '\n'.join(out) + '\n'
