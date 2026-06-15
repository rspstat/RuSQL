"""
RuSQL 벤치마크 결과 시각화
  - result.json 을 읽어 발표용 그래프 생성 (라이트 모드)
  - 실행: python graph.py
"""

import json
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec
from pathlib import Path

# ── 데이터 로드 ───────────────────────────────────────────────────────────
HERE = Path(__file__).parent
with open(HERE / "result.json", encoding="utf-8") as f:
    r = json.load(f)

s   = r["single"]
b   = r["bulk"]
pl  = r["point_lookup"]
qc  = r["query_cache"]

ins_s = s["rows"]  / s["insert_s"]
del_s = s["rows"]  / s["delete_s"]
ins_b = b["rows"]  / b["insert_s"]
del_b = b["rows"]  / b["delete_s"]

# ── 색상 ─────────────────────────────────────────────────────────────────
INS   = "#3B82F6"   # blue
DEL   = "#10B981"   # emerald
SEQ   = "#94A3B8"   # slate
IDX   = "#F97316"   # orange
CACHE = "#06B6D4"   # cyan
BG    = "#FFFFFF"
GRID  = "#E2E8F0"
TEXT  = "#0F172A"
SUB   = "#64748B"

# ── Figure ────────────────────────────────────────────────────────────────
plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Malgun Gothic", "AppleGothic", "NanumGothic",
                        "Arial Unicode MS", "DejaVu Sans"],
})

fig = plt.figure(figsize=(14, 9), facecolor=BG)
fig.text(0.5, 0.975, "RuSQL  ·  Performance Benchmark",
         ha="center", va="top", fontsize=22, fontweight="bold", color=TEXT)

gs = GridSpec(2, 2, figure=fig,
              left=0.07, right=0.97, top=0.90, bottom=0.07,
              hspace=0.48, wspace=0.34)
ax1 = fig.add_subplot(gs[0, 0])
ax2 = fig.add_subplot(gs[0, 1])
ax3 = fig.add_subplot(gs[1, 0])
ax4 = fig.add_subplot(gs[1, 1])


def style(ax, title, ylabel="", note=""):
    ax.set_facecolor(BG)
    ax.set_title(title, fontsize=12, fontweight="bold", color=TEXT,
                 loc="left", pad=10)
    if ylabel:
        ax.set_ylabel(ylabel, fontsize=9, color=SUB, labelpad=5)
    if note:
        ax.text(1, 1.015, note, transform=ax.transAxes,
                ha="right", va="bottom", fontsize=8, color=SUB)
    ax.tick_params(axis="both", colors=SUB, labelsize=9.5, length=0)
    ax.spines[["top", "right"]].set_visible(False)
    ax.spines["left"].set_color(GRID)
    ax.spines["bottom"].set_color(GRID)
    ax.yaxis.grid(True, color=GRID, linewidth=1.1, zorder=0)
    ax.set_axisbelow(True)


def vbar_label(ax, bar, fmt, ymax, unit="", color=TEXT, size=9.5):
    v = bar.get_height()
    ax.text(bar.get_x() + bar.get_width() / 2,
            v + ymax * 0.032,
            fmt.format(v) + unit,
            ha="center", va="bottom",
            fontsize=size, fontweight="bold", color=color)


W = 0.42

# ── ① 단건 쓰기 ──────────────────────────────────────────────────────────
ymax1 = max(ins_s, del_s) * 1.28
bi1 = ax1.bar(0, ins_s, W, color=INS, zorder=3)
bd1 = ax1.bar(1, del_s, W, color=DEL, zorder=3)
ax1.set_xticks([0, 1])
ax1.set_xticklabels(["INSERT", "DELETE"])
ax1.set_ylim(0, ymax1)
ax1.yaxis.set_major_formatter(plt.FuncFormatter(lambda x, _: f"{x:,.0f}"))
style(ax1, f"단건 쓰기  ({s['rows']:,} rows)", "rows / sec",
      f"{s['rows']:,}건 · 단건 I/O")
vbar_label(ax1, bi1[0], "{:,.0f}", ymax1, color=INS)
vbar_label(ax1, bd1[0], "{:,.0f}", ymax1, color=DEL)

# ── ② 묶음 쓰기 ──────────────────────────────────────────────────────────
ymax2 = max(ins_b, del_b) * 1.28
bi2 = ax2.bar(0, ins_b, W, color=INS, zorder=3)
bd2 = ax2.bar(1, del_b, W, color=DEL, zorder=3)
ax2.set_xticks([0, 1])
ax2.set_xticklabels(["INSERT", "DELETE"])
ax2.set_ylim(0, ymax2)
ax2.yaxis.set_major_formatter(plt.FuncFormatter(lambda x, _: f"{x:,.0f}"))
style(ax2, f"Bulk 쓰기  ({b['rows']:,} rows)", "rows / sec",
      f"{b['rows']:,}건 · 500행 묶음")
vbar_label(ax2, bi2[0], "{:,.0f}", ymax2, color=INS)
vbar_label(ax2, bd2[0], "{:,.0f}", ymax2, color=DEL)

# ── ③ 인덱스 성능 (포인트 조회) ───────────────────────────────────────────
seq_v = pl["seq_ms"]
idx_v = pl["idx_ms"]
sp    = pl["speedup"]

bs3 = ax3.bar(0, seq_v, W, color=SEQ, zorder=3)
bi3 = ax3.bar(1, idx_v, W, color=IDX, zorder=3)
ax3.set_xticks([0, 1])
ax3.set_xticklabels(["SeqScan", "B+Tree Index"])
ymax3 = max(seq_v, idx_v) * 1.40
ax3.set_ylim(0, ymax3)
style(ax3, "인덱스 성능  (ms / query  낮을수록 빠름)", "ms / query",
      "5,000행 · 포인트 조회 · 300회 평균")
ax3.text(bs3[0].get_x() + bs3[0].get_width()/2, seq_v + ymax3*0.03,
         f"{seq_v:.1f}", ha="center", va="bottom", fontsize=9, color=SUB)
ax3.text(bi3[0].get_x() + bi3[0].get_width()/2, idx_v + ymax3*0.03,
         f"{idx_v:.3f}", ha="center", va="bottom", fontsize=9, color="#C2410C")
ax3.text(0.5, ymax3 * 0.55, f"{sp:.0f}×",
         ha="center", va="bottom", fontsize=16, fontweight="bold", color="#9A3412",
         transform=ax3.get_xaxis_transform())

# ── ④ 쿼리 결과 캐시 ─────────────────────────────────────────────────────
scan_v = qc["scan_ms"]
hit_v  = qc["hit_ms"]
sp_qc  = qc["speedup"]

bp4 = ax4.bar(0, scan_v, W, color=SEQ,   zorder=3)
bn4 = ax4.bar(1, hit_v,  W, color=CACHE, zorder=3)
ax4.set_xticks([0, 1])
ax4.set_xticklabels(["DB Scan\n(캐시 미스)", "Cache Hit\n(캐시 히트)"])
ymax4 = max(scan_v, hit_v) * 1.35
ax4.set_ylim(0, ymax4)
style(ax4, "쿼리 캐시  (ms / query  낮을수록 빠름)", "ms / query",
      f"LRU 512 · {sp_qc:.0f}× speedup")
ax4.text(bp4[0].get_x() + bp4[0].get_width()/2, scan_v + ymax4*0.03,
         f"{scan_v:.2f}", ha="center", va="bottom", fontsize=9, color=SUB)
ax4.text(bn4[0].get_x() + bn4[0].get_width()/2, hit_v + ymax4*0.03,
         f"{hit_v:.3f}", ha="center", va="bottom", fontsize=9, color="#0E7490")
ax4.text(0.5, ymax4 * 0.55, f"{sp_qc:.0f}×",
         ha="center", va="bottom", fontsize=16, fontweight="bold", color=CACHE,
         transform=ax4.get_xaxis_transform())

# ── 저장 & 표시 ───────────────────────────────────────────────────────────
out = HERE / "benchmark_result.png"
plt.savefig(out, dpi=150, bbox_inches="tight", facecolor=BG)
print(f"저장됨: {out}")
plt.show()
