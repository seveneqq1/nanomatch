#!/usr/bin/env python3
"""Render a standalone HTML latency chart from nanomatch_bench JSON."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def render(data: dict) -> str:
    pct = data["percentiles"]
    hist = data["histogram"]
    counts = hist["counts"]
    bin_w = hist["bin_width_ns"]
    bins_js = ",".join(str(c) for c in counts)
    thr_m = float(data["throughput_ops_s"]) / 1e6
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>nanomatch latency</title>
<style>
  html, body {{ margin: 0; background: #1a120c; color: #efe4cc;
    font: 15px/1.45 "SF Mono", Menlo, Consolas, ui-monospace, monospace; }}
  body {{ min-height: 100vh; padding: 32px 28px 48px; }}
  h1 {{ font-weight: 500; font-size: 22px; letter-spacing: -0.02em; margin: 0 0 6px; }}
  .sub {{ color: #b8a888; margin-bottom: 28px; max-width: 68ch; }}
  .ticker {{ display: flex; flex-wrap: wrap; border: 1px solid #5a4630; margin-bottom: 24px; }}
  .ticker div {{ padding: 14px 18px; border-right: 1px solid #5a4630; min-width: 132px; }}
  .ticker div:last-child {{ border-right: 0; }}
  .ticker span {{ display: block; color: #b8a888; font-size: 11px; margin-bottom: 4px; }}
  .ticker strong {{ font-size: 20px; font-weight: 500; color: #ff7a45; }}
  canvas {{ width: 100%; height: 360px; background: #120c08; border: 1px solid #5a4630; display: block; }}
  figcaption {{ color: #b8a888; font-size: 12px; margin-top: 8px; }}
</style>
</head>
<body>
<h1>nanomatch · matching latency</h1>
<p class="sub">Rebuilt from <code>latency_report.json</code>. Copper columns are the
latency histogram; the pale line is the complementary CDF. Hairlines mark percentiles.</p>
<div class="ticker">
  <div><span>p50</span><strong>{pct["p50"]} ns</strong></div>
  <div><span>p90</span><strong>{pct["p90"]} ns</strong></div>
  <div><span>p99</span><strong>{pct["p99"]} ns</strong></div>
  <div><span>p99.9</span><strong>{pct["p99.9"]} ns</strong></div>
  <div><span>mean</span><strong>{data["mean"]} ns</strong></div>
  <div><span>throughput</span><strong>{thr_m:.2f} M/s</strong></div>
</div>
<canvas id="h" width="1400" height="360"></canvas>
<figcaption>min {data["min"]} ns · max {data["max"]} ns · {data["ops"]} ops ·
{data["trades"]} trades · {data["resting"]} resting · bin {bin_w} ns</figcaption>
<script>
const bins = [{bins_js}];
const p = {{p50:{pct["p50"]},p90:{pct["p90"]},p99:{pct["p99"]},p999:{pct["p99.9"]}}};
const binW = {bin_w};
const canvas = document.getElementById('h');
const ctx = canvas.getContext('2d');
const w = canvas.width, h = canvas.height;
const max = Math.max.apply(null, bins) || 1;
ctx.fillStyle = '#120c08'; ctx.fillRect(0,0,w,h);
ctx.strokeStyle = 'rgba(239,228,204,0.08)';
for (let i = 0; i < 6; i++) {{
  const y = (h-28)*i/5+8;
  ctx.beginPath(); ctx.moveTo(40,y); ctx.lineTo(w-8,y); ctx.stroke();
}}
const bw = (w-56)/bins.length;
ctx.fillStyle = '#ff7a45';
for (let i = 0; i < bins.length; i++) {{
  const bh = (bins[i]/max)*(h-48);
  ctx.fillRect(48+i*bw, h-24-bh, Math.max(1,bw-1), bh);
}}
let acc=0, total=bins.reduce((a,b)=>a+b,0)||1;
ctx.beginPath(); ctx.strokeStyle='#efe4cc'; ctx.lineWidth=1.5;
for (let i=0;i<bins.length;i++){{
  acc+=bins[i];
  const x=48+i*bw, y=8+(1-acc/total)*(h-48);
  if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
}}
ctx.stroke();
function mark(ns,label){{
  const x=48+(ns/binW)*bw;
  ctx.setLineDash([3,4]); ctx.strokeStyle='#efe4cc';
  ctx.beginPath(); ctx.moveTo(x,8); ctx.lineTo(x,h-24); ctx.stroke();
  ctx.setLineDash([]); ctx.fillStyle='#efe4cc';
  ctx.font='12px ui-monospace,monospace'; ctx.fillText(label,x+4,20);
}}
mark(p.p50,'p50'); mark(p.p90,'p90'); mark(p.p99,'p99'); mark(p.p999,'p99.9');
ctx.fillStyle='#b8a888'; ctx.font='11px ui-monospace,monospace';
ctx.fillText('latency →', w-90, h-8);
</script>
</body></html>
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-i", "--input", default="latency_report.json")
    parser.add_argument("-o", "--output", default="docs/latency.html")
    args = parser.parse_args()
    src = Path(args.input)
    dst = Path(args.output)
    data = json.loads(src.read_text())
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(render(data), encoding="utf-8")
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
