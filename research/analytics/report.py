"""Tearsheets (P5.9): one self-contained HTML page per run, and a comparison page across an experiment's runs.

    from research.analytics import report
    report.write_tearsheet(run, "tearsheet.html")          # a Results or a StoredRun
    report.write_comparison(result, experiment, "report.html")   # an ExperimentResult (research.experiments)

    python -m research.analytics.report RUN_ID [--results results] [--out FILE]

`python -m research.run` writes both for every experiment: results/experiments/<name>/report.html and
results/experiments/<name>/runs/<run_id>.html.

The pages need nothing but a browser: charts are inline SVG, with a few lines of inline script for the hover
read-outs, and every chart has its numbers in a table below it. They contain no times of their own and don't
say whether a run was run or taken from the store, so the same runs give byte-identical pages.

A tearsheet: the run's settings; headline figures; the mark-to-mid PnL with its drawdown; the PnL split
(spread capture, inventory, fees; research.analytics.pnl); the position; the markout curve (markouts); the
fill rate against the queue ahead (queue); and the TCA, risk and reject tables (tca, risk).

A comparison: every run with its parameters and headline metrics, linked to its tearsheet; and heatmaps of
the metrics over each pair of swept parameters (averaged over seeds and the other parameters), or one row of
cells when one parameter is swept.
"""

from __future__ import annotations

import argparse
import html
import itertools
import json
import math
import os
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence

import numpy as np

from research.analytics import markouts as _markouts
from research.analytics import pnl as _pnl
from research.analytics import queue as _queue
from research.analytics import risk as _risk
from research.analytics import tca as _tca
from research.analytics.market import MarketData, run_window

# ---- palette (the dataviz reference palette: categorical slots 1-4, the blue ramp, the diverging pair) ----

SERIES = ["--s1", "--s2", "--s3", "--s4"]
_BLUE = ["#cde2fb", "#b7d3f6", "#9ec5f4", "#86b6ef", "#6da7ec", "#5598e7", "#3987e5", "#2a78d6", "#256abf",
         "#1c5cab", "#184f95", "#104281", "#0d366b"]
_POS, _NEG, _MID = "#2a78d6", "#e34948", "#f0efec"

CSS = """
:root{color-scheme:light;--bg:#f9f9f7;--surface:#fcfcfb;--ink:#0b0b0b;--ink2:#52514e;--muted:#898781;
--grid:#e1e0d9;--axis:#c3c2b7;--ring:rgba(11,11,11,.10);--s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a;--s4:#eda100;
--wash:rgba(42,120,214,.10);--neg:#e34948}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){color-scheme:dark;--bg:#0d0d0d;
--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;--muted:#898781;--grid:#2c2c2a;--axis:#383835;
--ring:rgba(255,255,255,.10);--s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500;--wash:rgba(57,135,229,.12);
--neg:#e66767}}
:root[data-theme="dark"]{color-scheme:dark;--bg:#0d0d0d;--surface:#1a1a19;--ink:#f0efec;--ink2:#c3c2b7;
--muted:#898781;--grid:#2c2c2a;--axis:#383835;--ring:rgba(255,255,255,.10);--s1:#3987e5;--s2:#d95926;
--s3:#199e70;--s4:#c98500;--wash:rgba(57,135,229,.12);--neg:#e66767}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:1040px;margin:0 auto;padding:24px 16px 48px;min-width:0}
.card{min-width:0;overflow-wrap:anywhere}
h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:28px 0 8px}
.sub{color:var(--ink2);margin:0 0 16px}
.card{background:var(--surface);border:1px solid var(--ring);border-radius:8px;padding:16px;margin:12px 0}
.tiles{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(150px,100%),1fr));gap:12px;margin:12px 0}
.tile{background:var(--surface);border:1px solid var(--ring);border-radius:8px;padding:12px}
.tile .l{color:var(--ink2);font-size:12px}.tile .v{font-size:22px;font-weight:600}
.tile .n{color:var(--muted);font-size:12px}
.legend{display:flex;flex-wrap:wrap;gap:4px 16px;color:var(--ink2);font-size:12px;margin:0 0 6px}
.legend span{display:inline-flex;align-items:center;gap:6px}
.key{display:inline-block;width:14px;height:2px;border-radius:1px}
.keybox{display:inline-block;width:10px;height:10px;border-radius:2px}
svg{display:block;width:100%;height:auto;overflow:visible}
svg text{fill:var(--muted);font-size:11px;font-variant-numeric:tabular-nums}
svg .dl{fill:var(--ink2);font-size:11px}
.chart{position:relative}.chart:focus{outline:2px solid var(--s1);outline-offset:4px}
.tip{position:absolute;pointer-events:none;background:var(--surface);border:1px solid var(--ring);
border-radius:6px;padding:6px 8px;font-size:12px;box-shadow:0 2px 8px rgba(0,0,0,.12);display:none;
white-space:nowrap;z-index:2}
.tip .h{color:var(--ink2);margin-bottom:2px}.tip .r{display:flex;align-items:center;gap:6px}
.tip b{color:var(--ink)}
details{margin-top:8px}summary{color:var(--ink2);cursor:pointer;font-size:12px}
table{border-collapse:collapse;width:100%;font-size:12px;margin-top:6px}
th,td{text-align:right;padding:3px 8px;border-bottom:1px solid var(--grid);font-variant-numeric:tabular-nums;
white-space:nowrap;overflow-wrap:normal}
th:first-child,td:first-child{text-align:left}th{color:var(--ink2);font-weight:600}
.scroll{overflow-x:auto}
.kv{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(300px,100%),1fr));gap:0 24px}
.kv>div{min-width:0}
.kv div{display:flex;justify-content:space-between;gap:12px;border-bottom:1px solid var(--grid);padding:3px 0;font-size:13px}
.kv span:first-child{color:var(--ink2);flex:none}.kv span:last-child{font-variant-numeric:tabular-nums;text-align:right;overflow-wrap:anywhere;min-width:0}
a{color:var(--s1)}
.mark:hover,.mark:focus{opacity:.8;outline:none}
.scale{display:flex;align-items:center;gap:8px;color:var(--muted);font-size:11px;margin-top:4px}
.scale i{display:inline-block;width:160px;height:8px;border-radius:4px}
"""

JS = """
(function(){
function tipFor(ch){var t=ch.querySelector('.tip');if(!t){t=document.createElement('div');t.className='tip';ch.appendChild(t);}return t;}
function place(ch,t,x,y){var w=ch.clientWidth,tw=t.offsetWidth;t.style.left=Math.min(Math.max(x+12,0),w-tw-4)+'px';t.style.top=(y+12)+'px';}
function row(t,color,value,name){var r=document.createElement('div');r.className='r';var k=document.createElement('span');k.className='key';
k.style.background=color;var b=document.createElement('b');b.textContent=value;var n=document.createElement('span');n.textContent=name;
r.appendChild(k);r.appendChild(b);r.appendChild(n);t.appendChild(r);}
document.querySelectorAll('.chart[data-kind="line"]').forEach(function(ch){
 var d=JSON.parse(ch.querySelector('script').textContent),svg=ch.querySelector('svg'),cross=svg.querySelector('.cross'),t=tipFor(ch),idx=-1;
 function show(i,px,py){if(i<0||i>=d.px.length)return;idx=i;cross.setAttribute('x1',d.px[i]);cross.setAttribute('x2',d.px[i]);cross.style.display='';
  t.textContent='';var h=document.createElement('div');h.className='h';h.textContent=d.xl[i];t.appendChild(h);
  d.series.forEach(function(s){var v=s.y[i];row(t,getComputedStyle(ch).getPropertyValue(s.c),v===null?'-':v,s.n);});
  t.style.display='block';var r=svg.getBoundingClientRect(),sc=r.width/d.w;place(ch,t,px!==undefined?px:d.px[i]*sc,py!==undefined?py:8);}
 function hide(){t.style.display='none';cross.style.display='none';}
 svg.addEventListener('pointermove',function(e){var r=svg.getBoundingClientRect(),x=(e.clientX-r.left)*d.w/r.width,best=0,bd=1e18;
  for(var i=0;i<d.px.length;i++){var dd=Math.abs(d.px[i]-x);if(dd<bd){bd=dd;best=i;}}show(best,e.clientX-r.left,e.clientY-r.top);});
 svg.addEventListener('pointerleave',hide);ch.addEventListener('blur',hide);
 ch.addEventListener('keydown',function(e){if(e.key==='ArrowRight'){show(Math.min(idx+1,d.px.length-1));e.preventDefault();}
  else if(e.key==='ArrowLeft'){show(Math.max(idx-1,0));e.preventDefault();}else if(e.key==='Escape')hide();});
 ch.addEventListener('focus',function(){show(idx<0?d.px.length-1:idx);});
});
document.querySelectorAll('.chart[data-kind="marks"]').forEach(function(ch){var t=tipFor(ch);
 ch.querySelectorAll('.mark').forEach(function(m){
  function show(e){t.textContent='';var h=document.createElement('div');h.className='h';h.textContent=m.getAttribute('data-h');t.appendChild(h);
   var b=document.createElement('b');b.textContent=m.getAttribute('data-v');t.appendChild(b);t.style.display='block';
   var r=ch.getBoundingClientRect(),mr=m.getBoundingClientRect();place(ch,t,e&&e.clientX?e.clientX-r.left:mr.left-r.left+mr.width/2,e&&e.clientY?e.clientY-r.top:mr.top-r.top);}
  m.addEventListener('pointermove',show);m.addEventListener('focus',function(){show();});
  m.addEventListener('pointerleave',function(){t.style.display='none';});m.addEventListener('blur',function(){t.style.display='none';});});
});
})();
"""

W, H = 720, 260
ML, MR, MT, MB = 64, 96, 12, 28


# ---- formatting ----

def _e(s: Any) -> str:
    return html.escape(str(s), quote=True)


def fmt(v: Any, digits: int = 2, signed: bool = False) -> str:
    """A number for people: thousands separators, fixed decimals; '-' for missing."""
    if v is None:
        return "-"
    if isinstance(v, (bool, np.bool_)):
        return "yes" if v else "no"
    if isinstance(v, (int, np.integer)):
        return f"{int(v):+,}" if signed else f"{int(v):,}"
    try:
        x = float(v)
    except (TypeError, ValueError):
        return str(v)
    if math.isnan(x) or math.isinf(x):
        return "-"
    if x == 0:
        x = 0.0  # no -0
    return f"{x:+,.{digits}f}" if signed else f"{x:,.{digits}f}"



def _utc(ns: int, seconds: bool = True) -> str:
    t = datetime.fromtimestamp(ns / 1e9, tz=timezone.utc)
    return t.strftime("%H:%M:%S" if seconds else "%H:%M")


def _utc_full(ns: int) -> str:
    return datetime.fromtimestamp(ns / 1e9, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")


def _nice_ticks(lo: float, hi: float, n: int = 5) -> list[float]:
    if not (math.isfinite(lo) and math.isfinite(hi)):
        return [0.0]
    if hi - lo < 1e-12:
        pad = abs(hi) * 0.1 or 1.0
        lo, hi = lo - pad, hi + pad
    raw = (hi - lo) / n
    mag = 10 ** math.floor(math.log10(raw))
    step = next(m * mag for m in (1, 2, 2.5, 5, 10) if m * mag >= raw)
    start = math.floor(lo / step) * step
    ticks = []
    v = start
    while v <= hi + step * 1e-9:
        ticks.append(round(v, 12))
        v += step
    if ticks[-1] < hi:
        ticks.append(round(ticks[-1] + step, 12))
    return ticks


def _tick_label(v: float, ticks: Sequence[float]) -> str:
    step = abs(ticks[1] - ticks[0]) if len(ticks) > 1 else 1.0
    decimals = max(0, -math.floor(math.log10(step))) if step < 1 else 0
    if abs(v) >= 10_000 and step >= 1000:
        return f"{v / 1000:,.0f}K"
    return f"{v:,.{decimals}f}"


def _time_axis(t0: int, t1: int) -> list[tuple[int, str]]:
    """(position, label) ticks for a time axis: HH:MM, or HH:MM:SS when ticks are under a minute apart."""
    ticks = _time_ticks(t0, t1)
    secs = len(ticks) > 1 and ticks[1] - ticks[0] < 60_000_000_000
    return [(v, _utc(v, secs)) for v in ticks]


def auto_every(span_ns: int, min_buckets: int = 30) -> str:
    """The longest of 1 s, 5 s, 10 s, 30 s, 1 m, 5 m, 15 m, 1 h that gives at least min_buckets buckets."""
    best = "1s"
    for label, ns in (("1s", 1), ("5s", 5), ("10s", 10), ("30s", 30), ("1m", 60), ("5m", 300), ("15m", 900),
                      ("1h", 3600)):
        if span_ns / (ns * 1_000_000_000) >= min_buckets:
            best = label
    return best


def _time_ticks(t0: int, t1: int, n: int = 6) -> list[int]:
    span = max(t1 - t0, 1)
    steps = [s * 1_000_000_000 for s in (1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800,
                                          21600, 43200, 86400)]
    step = next((s for s in steps if span / s <= n), steps[-1])
    first = -(-t0 // step) * step
    return list(range(first, t1 + 1, step))


def _downsample(n: int, limit: int = 1200) -> np.ndarray:
    if n <= limit:
        return np.arange(n)
    idx = np.linspace(0, n - 1, limit).round().astype(int)
    return np.unique(idx)


# ---- charts ----

def _table(headers: Sequence[str], rows: Sequence[Sequence[Any]], caption: str = "Table") -> str:
    head = "".join(f"<th>{_e(h)}</th>" for h in headers)
    body = "".join("<tr>" + "".join(f"<td>{_e(c)}</td>" for c in r) + "</tr>" for r in rows)
    return (f'<details><summary>{_e(caption)}</summary><div class="scroll"><table><thead><tr>{head}</tr></thead>'
            f"<tbody>{body}</tbody></table></div></details>")


def _legend(names: Sequence[str], box: bool = False) -> str:
    if len(names) < 2:
        return ""
    cls = "keybox" if box else "key"
    return '<div class="legend">' + "".join(
        f'<span><i class="{cls}" style="background:var({SERIES[i]})"></i>{_e(n)}</span>'
        for i, n in enumerate(names)) + "</div>"


def line_chart(x: Sequence[float], series: Sequence[tuple[str, Sequence[float]]], *, x_labels: Sequence[str],
               x_ticks: Sequence[tuple[float, str]], y_title: str, digits: int = 2, area: bool = False,
               table_x: str = "x", table_note: str = "") -> str:
    """A line chart with a shared x: up to 4 series, a zero line when the range spans 0, a crosshair read-out,
    direct end labels when they don't collide, and the numbers in a table."""
    x = np.asarray(x, dtype=np.float64)
    ys = [np.asarray(v, dtype=np.float64) for _, v in series]
    finite = np.concatenate([v[np.isfinite(v)] for v in ys]) if ys else np.zeros(0)
    lo, hi = (float(finite.min()), float(finite.max())) if len(finite) else (0.0, 1.0)
    if area:
        lo, hi = min(lo, 0.0), max(hi, 0.0)
    ticks = _nice_ticks(lo, hi)
    y0, y1 = ticks[0], ticks[-1]
    x0, x1 = (float(x.min()), float(x.max())) if len(x) else (0.0, 1.0)
    if x1 == x0:
        x1 = x0 + 1
    pw, ph = W - ML - MR, H - MT - MB

    def sx(v):
        return ML + (v - x0) / (x1 - x0) * pw

    def sy(v):
        return MT + (y1 - v) / (y1 - y0) * ph

    parts = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="{_e(y_title)}">']
    for t in ticks:
        parts.append(f'<line x1="{ML}" x2="{W - MR}" y1="{sy(t):.1f}" y2="{sy(t):.1f}" stroke="var(--grid)" '
                     f'stroke-width="1"/>')
        parts.append(f'<text x="{ML - 6}" y="{sy(t) + 4:.1f}" text-anchor="end">{_e(_tick_label(t, ticks))}</text>')
    if y0 < 0 < y1:
        parts.append(f'<line x1="{ML}" x2="{W - MR}" y1="{sy(0):.1f}" y2="{sy(0):.1f}" stroke="var(--axis)" '
                     f'stroke-width="1"/>')
    parts.append(f'<line x1="{ML}" x2="{W - MR}" y1="{MT + ph}" y2="{MT + ph}" stroke="var(--axis)" stroke-width="1"/>')
    for tv, tl in x_ticks:
        parts.append(f'<text x="{sx(tv):.1f}" y="{H - 8}" text-anchor="middle">{_e(tl)}</text>')
    idx = _downsample(len(x))
    ends = []
    for k, ((name, _), v) in enumerate(zip(series, ys)):
        pts = [(sx(x[i]), sy(v[i])) for i in idx if np.isfinite(v[i])]
        if not pts:
            continue
        if len(pts) == 1:
            parts.append(f'<circle cx="{pts[0][0]:.1f}" cy="{pts[0][1]:.1f}" r="4" fill="var({SERIES[k]})"/>')
        d = "M" + "L".join(f"{a:.1f},{b:.1f}" for a, b in pts)
        if area:
            base = sy(0.0)
            parts.append(f'<path d="{d}L{pts[-1][0]:.1f},{base:.1f}L{pts[0][0]:.1f},{base:.1f}Z" '
                         f'fill="var({SERIES[k]})" fill-opacity=".10" stroke="none"/>')
        parts.append(f'<path d="{d}" fill="none" stroke="var({SERIES[k]})" stroke-width="2" '
                     f'stroke-linejoin="round" stroke-linecap="round"/>')
        ends.append((pts[-1][1], name, pts[-1]))
    # Direct end labels, unless two would collide
    ys_end = sorted(e[0] for e in ends)
    if len(ends) <= 4 and all(b - a >= 13 for a, b in zip(ys_end, ys_end[1:])):
        for ey, name, (px, py) in ends:
            parts.append(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="4" fill="var({SERIES[[e[1] for e in ends].index(name)]})" '
                         f'stroke="var(--surface)" stroke-width="2"/>')
            parts.append(f'<text class="dl" x="{px + 8:.1f}" y="{py + 4:.1f}">{_e(name)}</text>')
    parts.append(f'<line class="cross" x1="0" x2="0" y1="{MT}" y2="{MT + ph}" stroke="var(--muted)" '
                 f'stroke-width="1" style="display:none"/>')
    parts.append(f'<rect x="{ML}" y="{MT}" width="{pw}" height="{ph}" fill="transparent"/></svg>')
    data = {"w": W, "px": [round(sx(x[i]), 1) for i in idx], "xl": [x_labels[i] for i in idx],
            "series": [{"n": name, "c": SERIES[k],
                        "y": [fmt(v[i], digits) if np.isfinite(v[i]) else None for i in idx]}
                       for k, ((name, _), v) in enumerate(zip(series, ys))]}
    rows = [[x_labels[i], *[fmt(v[i], digits) for v in ys]] for i in idx]
    note = f" ({len(idx):,} of {len(x):,} points)" if len(idx) < len(x) else ""
    return (_legend([n for n, _ in series]) +
            f'<div class="chart" data-kind="line" tabindex="0" aria-label="{_e(y_title)}; arrow keys move the read-out">'
            + "".join(parts) + f'<script type="application/json">{json.dumps(data)}</script></div>'
            + _table([table_x, *[n for n, _ in series]], rows, f"Table{note}{table_note}"))


def bar_chart(labels: Sequence[str], values: Sequence[float], *, y_title: str, digits: int = 2,
              percent: bool = False, x_title: str = "", extra: Sequence[tuple[str, Sequence[Any]]] = ()) -> str:
    """Columns from one baseline (one series): <= 24 px wide, a 4 px rounded cap, value on hover and in the table."""
    v = np.asarray(values, dtype=np.float64)
    lo, hi = min(0.0, float(np.nanmin(v))) if len(v) else 0.0, max(0.0, float(np.nanmax(v))) if len(v) else 1.0
    ticks = _nice_ticks(lo, hi)
    y0, y1 = ticks[0], ticks[-1]
    pw, ph = W - ML - 16, H - MT - MB - 14
    band = pw / max(len(v), 1)
    bw = min(24.0, band * 0.6)

    def sy(t):
        return MT + (y1 - t) / (y1 - y0) * ph

    show = (lambda t: f"{t * 100:,.{max(digits - 2, 0)}f}%") if percent else (lambda t: fmt(t, digits))
    parts = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="{_e(y_title)}">']
    for t in ticks:
        parts.append(f'<line x1="{ML}" x2="{W - 16}" y1="{sy(t):.1f}" y2="{sy(t):.1f}" stroke="var(--grid)"/>')
        label = f"{t * 100:,.0f}%" if percent else _tick_label(t, ticks)
        parts.append(f'<text x="{ML - 6}" y="{sy(t) + 4:.1f}" text-anchor="end">{_e(label)}</text>')
    base = sy(0.0)
    parts.append(f'<line x1="{ML}" x2="{W - 16}" y1="{base:.1f}" y2="{base:.1f}" stroke="var(--axis)"/>')
    for i, (lab, val) in enumerate(zip(labels, v)):
        cx = ML + band * (i + 0.5)
        if np.isfinite(val) and val != 0:
            top, bot = (sy(val), base) if val > 0 else (base, sy(val))
            h = max(bot - top, 1.0)
            r = min(4.0, h, bw / 2)
            x0 = cx - bw / 2
            if val > 0:  # rounded at the data end (top), square at the baseline
                d = (f"M{x0:.1f},{bot:.1f}V{top + r:.1f}Q{x0:.1f},{top:.1f} {x0 + r:.1f},{top:.1f}"
                     f"H{x0 + bw - r:.1f}Q{x0 + bw:.1f},{top:.1f} {x0 + bw:.1f},{top + r:.1f}V{bot:.1f}Z")
            else:
                d = (f"M{x0:.1f},{top:.1f}V{bot - r:.1f}Q{x0:.1f},{bot:.1f} {x0 + r:.1f},{bot:.1f}"
                     f"H{x0 + bw - r:.1f}Q{x0 + bw:.1f},{bot:.1f} {x0 + bw:.1f},{bot - r:.1f}V{top:.1f}Z")
            parts.append(f'<path d="{d}" fill="var(--s1)"/>')
        parts.append(f'<rect class="mark" tabindex="0" x="{cx - band / 2:.1f}" y="{MT}" width="{band:.1f}" '
                     f'height="{ph}" fill="transparent" data-h="{_e(lab)}" data-v="{_e(show(val))}"/>')
        parts.append(f'<text x="{cx:.1f}" y="{H - 22}" text-anchor="middle" style="font-size:10px">{_e(lab)}</text>')
    if x_title:
        parts.append(f'<text x="{ML + pw / 2:.1f}" y="{H - 4}" text-anchor="middle">{_e(x_title)}</text>')
    parts.append("</svg>")
    rows = [[lab, show(val), *[fmt(col[i], digits) if not isinstance(col[i], str) else col[i] for _, col in extra]]
            for i, (lab, val) in enumerate(zip(labels, v))]
    return (f'<div class="chart" data-kind="marks">' + "".join(parts) + "</div>"
            + _table([x_title or "", y_title, *[n for n, _ in extra]], rows))


def _mix(a: str, b: str, t: float) -> str:
    ca = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
    cb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(ca, cb))


def _cell_color(v: float, lo: float, hi: float, diverging: bool) -> str:
    if not math.isfinite(v):
        return "transparent"
    if diverging:
        m = max(abs(lo), abs(hi)) or 1.0
        t = min(abs(v) / m, 1.0)
        return _mix(_MID, _POS if v >= 0 else _NEG, t)
    t = 0.0 if hi == lo else (v - lo) / (hi - lo)
    pos = t * (len(_BLUE) - 1)
    i = min(int(pos), len(_BLUE) - 2)
    return _mix(_BLUE[i], _BLUE[i + 1], pos - i)


def heatmap(x_name: str, x_labels: Sequence[str], y_name: str, y_labels: Sequence[str], grid: np.ndarray, *,
            title: str, digits: int = 2, diverging: bool = False) -> str:
    """A grid of cells (rows: y values, columns: x values): blue for magnitude, or blue / gray / red around 0;
    each cell's value on hover and in the table."""
    nx, ny = len(x_labels), len(y_labels)
    lw = 140
    cw = max(48.0, min(170.0, (W - lw - 16) / max(nx, 1)))

    def fit(label: str, room: float) -> str:  # the whole label is in the tooltip and the table
        n = max(int(room / 6.4), 3)
        return label if len(label) <= n else label[: n - 1] + "…"

    ch = 34.0
    width = lw + cw * nx + 16
    height = 20 + ch * ny + 34
    vals = grid[np.isfinite(grid)]
    lo, hi = (float(vals.min()), float(vals.max())) if len(vals) else (0.0, 1.0)
    parts = [f'<svg viewBox="0 0 {width:.0f} {height:.0f}" role="img" aria-label="{_e(title)}" '
             f'style="max-width:{width:.0f}px">']
    for j, yl in enumerate(y_labels):
        parts.append(f'<text x="{lw - 8}" y="{20 + ch * j + ch / 2 + 4:.1f}" text-anchor="end">{_e(fit(yl, lw - 12))}</text>')
        for i, xl in enumerate(x_labels):
            v = grid[j, i]
            x, y = lw + cw * i, 20 + ch * j
            parts.append(f'<rect class="mark" tabindex="0" x="{x + 1:.1f}" y="{y + 1:.1f}" width="{cw - 2:.1f}" '
                         f'height="{ch - 2:.1f}" rx="4" fill="{_cell_color(v, lo, hi, diverging)}" '
                         f'stroke="var(--ring)" data-h="{_e(f"{y_name} {yl}, {x_name} {xl}")}" '
                         f'data-v="{_e(fmt(v, digits))}"/>')
    for i, xl in enumerate(x_labels):
        parts.append(f'<text x="{lw + cw * i + cw / 2:.1f}" y="{20 + ch * ny + 14:.1f}" '
                     f'text-anchor="middle">{_e(fit(xl, cw - 8))}</text>')
    parts.append(f'<text x="{lw + cw * nx / 2:.1f}" y="{20 + ch * ny + 30:.1f}" text-anchor="middle">'
                 f'{_e(x_name)}</text>')
    parts.append(f'<text x="{lw - 8}" y="12" text-anchor="end">{_e(y_name)}</text></svg>')
    if diverging:
        m = max(abs(lo), abs(hi))
        scale = (f'<div class="scale"><span>{_e(fmt(-m, digits))}</span><i style="background:linear-gradient(90deg,'
                 f'{_NEG},{_MID},{_POS})"></i><span>{_e(fmt(m, digits, signed=True))}</span></div>')
    else:
        scale = (f'<div class="scale"><span>{_e(fmt(lo, digits))}</span><i style="background:linear-gradient(90deg,'
                 f'{_BLUE[0]},{_BLUE[6]},{_BLUE[-1]})"></i><span>{_e(fmt(hi, digits))}</span></div>')
    rows = [[yl, *[fmt(grid[j, i], digits) for i in range(nx)]] for j, yl in enumerate(y_labels)]
    return (f'<div class="chart" data-kind="marks">' + "".join(parts) + "</div>" + scale
            + _table([f"{y_name} \\ {x_name}", *x_labels], rows))


def _tiles(items: Sequence[tuple[str, str, str]]) -> str:
    return '<div class="tiles">' + "".join(
        f'<div class="tile"><div class="l">{_e(l)}</div><div class="v">{_e(v)}</div>'
        f'<div class="n">{_e(n)}</div></div>' for l, v, n in items) + "</div>"


def _kv(pairs: Sequence[tuple[str, str]]) -> str:
    return '<div class="kv">' + "".join(f"<div><span>{_e(k)}</span><span>{_e(v)}</span></div>"
                                        for k, v in pairs) + "</div>"


def _page(title: str, body: str) -> str:
    return (f'<!doctype html><html lang="en"><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width,initial-scale=1"><title>{_e(title)}</title>'
            f"<style>{CSS}</style></head><body><main>{body}</main><script>{JS}</script></body></html>\n")


# ---- a run ----

def _short(v: Any) -> str:
    if isinstance(v, float):
        return f"{v:.6g}"
    return v if isinstance(v, str) else json.dumps(v, separators=(",", ":"), sort_keys=True)


def metrics(run: Any, market: MarketData | None = None, every: str | None = None) -> dict[str, Any]:
    """The headline numbers of a run, per strategy (the first one): what the comparison tabulates. every: the
    bucket for the Sharpe ratio (None: auto_every of the run's length)."""
    market = market or MarketData.for_run(run)
    every = every or auto_every(run_window(run)[2] - run_window(run)[1])
    d = _pnl.decompose(run, market).row(0, named=True)
    t = _tca.summary(run, "5s", market).row(0, named=True)
    r = _risk.summary(run, every, market=market).row(0, named=True)
    mo = _markouts.markout_curve(run, ["5s"], by=(), market=market)
    return {
        "pnl": d["pnl"], "spread_capture": d["spread_capture"], "inventory": d["inventory"], "fees": d["fees"],
        "fills": d["fills"], "base_volume": d["base_volume"], "fill_ratio": t["fill_ratio"],
        "markout_5s_bps": float(mo["mean_bps"][0]) if mo.height else float("nan"),
        "max_drawdown": r["max_drawdown"], "sharpe": r["sharpe"], "inv_mean_abs": r["inv_mean_abs"],
        "ttf_p50_ms": t["ttf_p50_ms"],
    }


def tearsheet(run: Any, market: MarketData | None = None, every: str | None = None, title: str | None = None,
              back_link: str | None = None) -> str:
    """The tearsheet of a run (Results or StoredRun) as an HTML string. every: the PnL bucket length (None: the
    longest that gives at least 30 buckets, auto_every)."""
    import polars as pl

    market = market or MarketData.for_run(run)
    every = every or auto_every(run_window(run)[2] - run_window(run)[1])
    meta = run.meta
    cfg = meta.get("config", {})
    strat = cfg.get("strategy", {})
    owner = strat.get("owner") or _risk._owners(run)[0]
    start, trading, end = run_window(run)
    d = _pnl.decompose(run, market).filter(pl.col("owner") == owner).row(0, named=True)
    buckets = _pnl.pnl_buckets(run, every, market, owner)
    t = _tca.summary(run, "5s", market).filter(pl.col("owner") == owner).row(0, named=True)
    r = _risk.summary(run, every, market=market).filter(pl.col("owner") == owner).row(0, named=True)
    dd = _risk.drawdown_series(run).filter(pl.col("owner") == owner)
    pos = _risk.table(run, "positions").filter((pl.col("owner") == owner) & (pl.col("ts_ns") >= trading))
    curve = _markouts.markout_curve(run, market=market).filter(pl.col("owner") == owner)
    rejects = _tca.rejects(run).filter(pl.col("owner") == owner)
    try:
        qp = _queue.placements(run, market).filter(pl.col("owner") == owner)
    except ValueError:
        qp = None

    # A Python strategy: run_meta.json names it (python_strategy); a stored run's identity has its params
    name, params = strat.get("name", ""), strat.get("params", {})
    if isinstance(meta.get("python_strategy"), str):
        name = meta["python_strategy"]
        params = ((meta.get("identity") or {}).get("python_strategy") or {}).get("params", {})
    run_id = meta.get("run_id", "")
    head = title or f"{name} on {meta.get('data', {}).get('symbol', cfg.get('symbol', ''))}"
    body = []
    if back_link:
        body.append(f'<p class="sub"><a href="{_e(back_link)}">&larr; experiment report</a></p>')
    body.append(f"<h1>{_e(head)}</h1>")
    body.append(f'<p class="sub">{_e(_utc_full(trading))} to {_e(_utc_full(end))}'
                f'{" · run " + _e(run_id) if run_id else ""}</p>')
    lat = cfg.get("latency_preset") or "custom latency"
    qm = cfg.get("queue_model", {})
    settings = [("strategy", f"{name} {_short(params)}"), ("owner", owner),
                ("dataset", str(meta.get("data", {}).get("file", cfg.get("dataset", "")))),
                ("window", f"{_utc_full(start)} + warm-up {(trading - start) / 1e9:g} s"),
                ("latency", lat), ("queue model", _short(qm)), ("seed", str(cfg.get("seed", ""))),
                ("fees", _short(cfg.get("fees", {}))), ("output hash", meta.get("output_hash", "")[:16])]
    if meta.get("experiment"):
        settings.insert(0, ("experiment point", _short(meta["experiment"].get("params", {}))))
    body.append(_kv(settings))

    body.append(_tiles([
        ("PnL", fmt(d["pnl"], 2, signed=True), "USDT, mark-to-mid"),
        ("Spread capture", fmt(d["spread_capture"], 2, signed=True), f"{fmt(d['edge_bps'], 2, signed=True)} bps of notional"),
        ("Inventory", fmt(d["inventory"], 2, signed=True), "USDT"),
        ("Fees", fmt(-d["fees"], 2, signed=True), "USDT"),
        ("Fills", fmt(d["fills"]), f"{fmt(d['maker_fills'])} maker · {fmt(d['taker_fills'])} taker"),
        ("Fill ratio", fmt(t["fill_ratio"] * 100 if t["fill_ratio"] == t["fill_ratio"] else None, 1) + "%",
         f"time to fill p50 {fmt(t['ttf_p50_ms'] / 1000 if t['ttf_p50_ms'] == t['ttf_p50_ms'] else None, 2)} s"),
        ("Max drawdown", fmt(r["max_drawdown"], 2), "USDT"),
        ("Markout 5 s", fmt(curve.filter(pl.col("horizon") == "5s")["mean_bps"].mean() if curve.height else None,
                            2, signed=True), "bps, all fills"),
    ]))

    # PnL and drawdown (two charts, one axis each)
    ts = dd["ts_ns"].to_numpy()
    if len(ts):
        xt = _time_axis(int(ts[0]), int(ts[-1]))
        labels = [_utc(int(v)) for v in ts]
        body.append('<div class="card"><h2>PnL, mark-to-mid (USDT)</h2>' + line_chart(
            ts, [("PnL", dd["pnl"].to_numpy())], x_labels=labels, x_ticks=xt, y_title="PnL (USDT)", area=True,
            table_x="time (UTC)") + "</div>")
        body.append('<div class="card"><h2>Drawdown from the running high (USDT)</h2>' + line_chart(
            ts, [("Drawdown", -dd["drawdown"].to_numpy())], x_labels=labels, x_ticks=xt, y_title="drawdown (USDT)",
            area=True, table_x="time (UTC)") + "</div>")
    # The PnL split, cumulative per bucket
    if buckets.height:
        be = np.concatenate([[int(buckets["start_ns"][0])], buckets["end_ns"].to_numpy()])

        def cum(col: str, sign: float = 1.0) -> np.ndarray:
            return np.concatenate([[0.0], sign * np.cumsum(buckets[col].to_numpy())])

        xt = _time_axis(int(be[0]), int(be[-1]))
        body.append(f'<div class="card"><h2>PnL split, cumulative (USDT, {_e(every)} buckets)</h2>' + line_chart(
            be, [("PnL", cum("pnl")), ("Spread capture", cum("spread_capture")), ("Inventory", cum("inventory")),
                 ("Fees", cum("fees", -1.0))],
            x_labels=[_utc(int(v)) for v in be], x_ticks=xt, y_title="USDT", table_x="bucket end (UTC)") + "</div>")
    # Position
    if pos.height:
        pt = pos["ts_ns"].to_numpy()
        xt = _time_axis(int(pt[0]), int(pt[-1]))
        body.append('<div class="card"><h2>Position (base units, at each sample)</h2>' + line_chart(
            pt, [("Position", pos["position"].to_numpy())], x_labels=[_utc(int(v)) for v in pt], x_ticks=xt,
            y_title="position", digits=5, table_x="time (UTC)") + _kv([
                ("time-weighted mean", fmt(r["inv_mean"], 5)), ("mean |position|", fmt(r["inv_mean_abs"], 5)),
                ("long / short / flat", f"{fmt(r['inv_long'] * 100, 1)}% / {fmt(r['inv_short'] * 100, 1)}% / "
                                        f"{fmt(r['inv_flat'] * 100, 1)}%"),
                ("at the position limit", "-" if r["time_at_position_limit"] is None
                 else f"{fmt(r['time_at_position_limit'] * 100, 1)}% of the time")]) + "</div>")
    # Markouts
    if curve.height:
        groups = curve.select("liquidity", "side").unique(maintain_order=True).rows()[:4]
        hs = curve["horizon"].unique(maintain_order=True).to_list()
        series = []
        for liq, side in groups:
            g = curve.filter((pl.col("liquidity") == liq) & (pl.col("side") == side))
            vals = dict(zip(g["horizon"].to_list(), g["mean_bps"].to_list()))
            series.append((f"{liq} {side}", [vals.get(h, float("nan")) for h in hs]))
        body.append('<div class="card"><h2>Markouts (bps, quantity-weighted; positive is good)</h2>' + line_chart(
            list(range(len(hs))), series, x_labels=hs, x_ticks=list(enumerate(hs)), y_title="markout (bps)",
            digits=3, table_x="horizon", table_note=" (horizons evenly spaced, not to scale)") + "</div>")
    # Queue
    if qp is not None and qp.height:
        fc = _queue.fill_curve(run, placements_df=qp).filter(pl.col("owner") == owner)
        labs = [("new level" if b.startswith("0 (") else f"{lo:.2g}–{hi:.2g}")
                for b, lo, hi in zip(fc["bin"], fc["lo"], fc["hi"])]
        qs = _queue.summary(run, placements_df=qp).row(0, named=True)
        body.append('<div class="card"><h2>Fill rate by queue ahead at entry (passive placements)</h2>' + bar_chart(
            labs, fc["fill_rate"].to_list(), y_title="fill rate", percent=True, digits=3,
            x_title="queue ahead at entry (base units)",
            extra=[("placements", fc["placements"].to_list()),
                   ("median time to fill (ms)", fc["median_time_to_fill_ms"].to_list())]) + _kv([
                ("passive placements", fmt(qs["placements"])), ("joined the best price", f"{fmt(qs['at_best'] * 100, 1)}%"),
                ("median queue ahead", fmt(qs["median_ahead"], 5)),
                ("median time in queue", f"{fmt(qs['median_time_in_queue_ms'] / 1000, 2)} s")]) + "</div>")
    # Tables
    body.append('<div class="card"><h2>Transaction costs</h2>' + _kv([
        ("placements", fmt(t["placements"])), ("requests new / replace / cancel / amend",
                                                f"{t['new']} / {t['replace']} / {t['cancel']} / {t['amend']}"),
        ("filled / sent quantity", f"{fmt(t['filled_qty'], 5)} / {fmt(t['sent_qty'], 5)}"),
        ("time to first fill p50 / mean / p90 (ms)",
         f"{fmt(t['ttf_p50_ms'], 0)} / {fmt(t['ttf_mean_ms'], 0)} / {fmt(t['ttf_p90_ms'], 0)}"),
        ("implementation shortfall (bps)", fmt(t["shortfall_bps"], 3, signed=True)),
        ("opportunity cost (USDT)", fmt(t["opportunity_cost"], 2, signed=True)),
        ("effective / realized spread, 5 s (bps)",
         f"{fmt(t['effective_spread_bps'], 3, signed=True)} / {fmt(t['realized_spread_bps'], 3, signed=True)}"),
        ("price impact, 5 s (bps)", fmt(t["price_impact_bps"], 3, signed=True)),
        ("cancels per fill", fmt(t["cancel_fill_ratio"], 3)), ("requests per fill", fmt(t["order_to_trade"], 3)),
        ("rejected orders / refused requests", f"{t['rejected']} / {t['refused_requests']}"),
    ]) + (_table(["kind", "reason", "count"], rejects.select("kind", "reason", "count").rows(), "Rejects by reason")
          if rejects.height else "") + "</div>")
    body.append('<div class="card"><h2>Risk</h2>' + _kv([
        (f"Sharpe / Sortino ({every} buckets, annualized)", f"{fmt(r['sharpe'], 2)} / {fmt(r['sortino'], 2)}"),
        ("buckets / with a gain", f"{r['buckets']} / {fmt(r['hit_rate'] * 100 if r['hit_rate'] == r['hit_rate'] else None, 1)}%"),
        ("best / worst bucket (USDT)", f"{fmt(r['best'], 2, signed=True)} / {fmt(r['worst'], 2, signed=True)}"),
        ("max drawdown (USDT) / duration", f"{fmt(r['max_drawdown'], 2)} / {fmt(r['duration_s'], 0)} s"),
        ("volume (base / quote)", f"{fmt(r['base_volume'], 5)} / {fmt(r['quote_volume'], 2)}"),
        ("quote volume per hour", fmt(r["quote_volume_per_hour"], 2)),
        ("inventory turnover / holding time", f"{fmt(r['inventory_turnover'], 1)} / {fmt(r['holding_time_s'], 1)} s"),
        ("open-order limit samples", "-" if r["samples_at_open_orders_limit"] is None
         else f"{fmt(r['samples_at_open_orders_limit'] * 100, 1)}%"),
    ]) + "</div>")
    return _page(head, "".join(body))


def write_tearsheet(run: Any, path: str | Path, **kw: Any) -> Path:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    _write(path, tearsheet(run, **kw))
    return path


def _write(path: Path, text: str) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


# ---- an experiment ----

METRICS = [  # (key, title, digits, diverging)
    ("pnl", "PnL (USDT)", 2, True),
    ("fills", "Fills", 0, False),
    ("markout_5s_bps", "Markout at 5 s (bps)", 3, True),
    ("max_drawdown", "Max drawdown (USDT)", 2, False),
]


def comparison(result: Any, experiment: Any, rows: Sequence[dict[str, Any]], run_links: dict[str, str]) -> str:
    """The comparison page: rows are {"record": RunRecord, "metrics": metrics(...) or None}."""
    swept = list(experiment.sweep.params) if experiment.sweep else []

    def pname(key: str) -> str:  # strategy.params.refresh_ms -> refresh_ms
        return key.removeprefix("strategy.params.")
    body = [f"<h1>{_e(experiment.name)}</h1>"]
    if experiment.description:
        body.append(f'<p class="sub">{_e(experiment.description)}</p>')
    n_failed = len(result.failed)  # not ran / cached: the page shows results, the same however they were obtained
    body.append(f'<p class="sub">{len(result.runs)} runs{f", {n_failed} failed" if n_failed else ""}'
                f' · dataset {_e(experiment.dataset)} · seeds {_e(", ".join(map(str, experiment.seeds)))}</p>')
    ok = [r for r in rows if r["metrics"] is not None]
    # Heatmaps over pairs of swept parameters (or one row for a single parameter)
    if swept and ok:
        pairs = list(itertools.combinations(swept, 2))[:3] if len(swept) >= 2 else [(swept[0], None)]
        for xk, yk in pairs:
            # Every point, failed ones too (an empty cell)
            xs = list(dict.fromkeys(_short(r["record"].params[xk]) for r in rows))
            ys = list(dict.fromkeys(_short(r["record"].params[yk]) for r in rows)) if yk else ["all"]
            for key, title, digits, div in METRICS:
                grid = np.full((len(ys), len(xs)), np.nan)
                for j, yv in enumerate(ys):
                    for i, xv in enumerate(xs):
                        vals = [float(r["metrics"][key]) for r in ok if _short(r["record"].params[xk]) == xv
                                and (yk is None or _short(r["record"].params[yk]) == yv)
                                and r["metrics"][key] is not None and math.isfinite(float(r["metrics"][key]))]
                        if vals:
                            grid[j, i] = float(np.mean(vals))
                note = " (mean over seeds" + (" and the other parameters)" if len(swept) > 2 else ")")
                body.append(f'<div class="card"><h2>{_e(title)}{_e(note)}</h2>' + heatmap(
                    pname(xk), xs, pname(yk) if yk else "", ys, grid, title=title, digits=digits, diverging=div)
                    + "</div>")
    # Every run
    head = ["run", *map(pname, swept), "seed", "PnL", "spread", "inventory", "fees", "fills", "fill ratio",
            "markout 5 s (bps)", "max drawdown", "Sharpe"]
    trs = []
    for r in rows:
        rec, m = r["record"], r["metrics"]
        link = run_links.get(rec.run_id)
        cell = f'<a href="{_e(link)}">{_e(rec.run_id)}</a>' if link else _e(rec.run_id)
        vals = [_short(rec.params.get(k)) for k in swept] + [str(rec.seed)]
        if m:
            vals += [fmt(m["pnl"], 2, True), fmt(m["spread_capture"], 2, True), fmt(m["inventory"], 2, True),
                     fmt(m["fees"], 2), fmt(m["fills"]), fmt(m["fill_ratio"], 3), fmt(m["markout_5s_bps"], 3, True),
                     fmt(m["max_drawdown"], 2), fmt(m["sharpe"], 1)]
        else:
            vals += ["failed"] + ["-"] * 8
        trs.append(f"<tr><td>{cell}</td>" + "".join(f"<td>{_e(v)}</td>" for v in vals) + "</tr>")
    body.append('<div class="card"><h2>Runs</h2><div class="scroll"><table><thead><tr>' +
                "".join(f"<th>{_e(h)}</th>" for h in head) + "</tr></thead><tbody>" + "".join(trs) +
                "</tbody></table></div></div>")
    failed = [r["record"] for r in rows if r["record"].status == "failed"]
    if failed:
        body.append('<div class="card"><h2>Failed runs</h2>' + "".join(
            f"<p><b>{_e(f.run_id)}</b> {_e(_short(f.params))} seed {f.seed}: {_e((f.error or '').splitlines()[0])}</p>"
            for f in failed) + "</div>")
    return _page(f"{experiment.name}: comparison", "".join(body))


def write_reports(result: Any, experiment: Any, out_dir: str | Path | None = None, every: str | None = None,
                  progress: Any = print) -> Path:
    """Every run's tearsheet (runs/<run_id>.html) and the comparison (report.html) in the experiment's directory
    (results/experiments/<name>/). Returns the comparison's path."""
    out = Path(out_dir) if out_dir else Path(experiment.results) / "experiments" / experiment.name
    rows, links = [], {}
    for rec in result.runs:
        m = None
        if rec.status in ("ran", "cached", "duplicate"):
            run = result.load(rec)
            market = MarketData.for_run(run)
            m = metrics(run, market, every)
            if rec.run_id not in links:
                write_tearsheet(run, out / "runs" / f"{rec.run_id}.html", market=market, every=every,
                                back_link="../report.html")
                links[rec.run_id] = f"runs/{rec.run_id}.html"
        rows.append({"record": rec, "metrics": m})
    path = out / "report.html"
    _write(path, comparison(result, experiment, rows, links))
    if progress:
        progress(f"{experiment.name}: report {path} ({len(links)} tearsheets)")
    return path


def main(argv: list[str] | None = None) -> int:
    from research.experiments.store import Store

    ap = argparse.ArgumentParser(prog="python -m research.analytics.report",
                                 description="Write the tearsheet of a stored run (HTML).")
    ap.add_argument("run_id")
    ap.add_argument("--results", default="results", help="the store's root (default results/)")
    ap.add_argument("--out", help="the HTML file (default results/<run_id>/../tearsheet-<run_id>.html)")
    ap.add_argument("--every", help="PnL bucket length (default: at least 30 buckets)")
    args = ap.parse_args(argv)
    run = Store(args.results).load(args.run_id)
    path = write_tearsheet(run, args.out or Path(args.results) / f"tearsheet-{args.run_id}.html", every=args.every)
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
