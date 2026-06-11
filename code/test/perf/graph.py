"""
RuSQL 벤치마크 결과 시각화
  - result.json 을 읽어 발표용 그래프 생성 (라이트 모드)
  - 실행: python graph.py
"""

import json
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.gridspec import GridSpec
from pathlib import Path

# ── 데이터 로드 ───────────────────────────────────────────────────────────
HERE = Path(__file__).parent
with open(HERE / "result.json", encoding="utf-8") as f:
    r = json.load(f)

s   = r["single"]
b   = r["bulk"]
pl  = r["point_lookup"]
rq  = r["range_query"]
tk  = r["top_k"]
par = r["parallel"]

ins_s = s["rows"]  / s["insert_s"]
del_s = s["rows"]  / s["delete_s"]
ins_b = b["rows"]  / b["insert_s"]
del_b = b["rows"]  / b["delete_s"]

# ── 색상 ─────────────────────────────────────────────────────────────────
INS   = "#3B82F6"   # blue
DEL   = "#10B981"   # emerald
SEQ   = "#94A3B8"   # slate
IDX   = "#F97316"   # orange
P_OFF = "#94A3B8"   # gray
P_ON  = "#8B5CF6"   # violet
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

fig = plt.figure(figsize=(17, 9.5), facecolor=BG)
fig.text(0.5, 0.97, "RuSQL  ·  Performance Benchmark",
         ha="center", va="top", fontsize=22, fontweight="bold", color=TEXT)
fig.text(0.5, 0.935, "In-memory B+Tree engine  |  dev build  |  Windows 11",
         ha="center", va="top", fontsize=11, color=SUB)

gs = GridSpec(2, 2, figure=fig,
              left=0.06, right=0.97, top=0.88, bottom=0.07,
              hspace=0.54, wspace=0.36)
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


def badge(ax, text, color, bg, border):
    ax.text(0.97, 0.96, text, transform=ax.transAxes,
            ha="right", va="top", fontsize=10, color=color,
            bbox=dict(boxstyle="round,pad=0.35", facecolor=bg,
                      edgecolor=border, linewidth=1.2))


# ── ① 단건 쓰기 ──────────────────────────────────────────────────────────
W = 0.42
ymax1 = max(ins_s, del_s) * 1.28
bi1 = ax1.bar(0, ins_s, W, color=INS, zorder=3)
bd1 = ax1.bar(1, del_s, W, color=DEL, zorder=3)
ax1.set_xticks([0, 1])
ax1.set_xticklabels(["INSERT", "DELETE"])
ax1.set_ylim(0, ymax1)
ax1.yaxis.set_major_formatter(plt.FuncFormatter(lambda x, _: f"{x:,.0f}"))
style(ax1, "단건 쓰기  (10,000 rows)", "rows / sec", "단건 I/O · 1행씩")
vbar_label(ax1, bi1[0], "{:,.0f}", ymax1, color=INS)
vbar_label(ax1, bd1[0], "{:,.0f}", ymax1, color=DEL)
badge(ax1, "INSERT ≈ DELETE", SUB, "#F8FAFC", GRID)

# ── ② 묶음 쓰기 ──────────────────────────────────────────────────────────
ymax2 = max(ins_b, del_b) * 1.28
bi2 = ax2.bar(0, ins_b, W, color=INS, zorder=3)
bd2 = ax2.bar(1, del_b, W, color=DEL, zorder=3)
ax2.set_xticks([0, 1])
ax2.set_xticklabels(["INSERT", "DELETE"])
ax2.set_ylim(0, ymax2)
ax2.yaxis.set_major_formatter(plt.FuncFormatter(lambda x, _: f"{x:,.0f}"))
style(ax2, "묶음 쓰기  (100,000 rows)", "rows / sec", "500행 묶음 · swap_remove")
vbar_label(ax2, bi2[0], "{:,.0f}", ymax2, color=INS)
vbar_label(ax2, bd2[0], "{:,.0f}", ymax2, color=DEL)
ratio_b = del_b / ins_b
badge(ax2, f"DELETE  {ratio_b:.1f}× faster",
      "#047857", "#F0FDF4", "#BBF7D0")

# ── ③ 인덱스 최적화 ───────────────────────────────────────────────────────
labels3  = ["포인트 조회\n(등호)", "범위 쿼리\n(BETWEEN)", "Top-K\n(ORDER BY LIMIT)"]
seq_vals = [pl["seq_ms"], rq["seq_ms"], tk["seq_ms"]]
idx_vals = [pl["idx_ms"], rq["idx_ms"], tk["idx_ms"]]
speedups = [pl["speedup"], rq["speedup"], tk["speedup"]]

x3 = np.arange(3)
w3 = 0.33
bs3 = ax3.bar(x3 - w3/2, seq_vals, w3, color=SEQ, zorder=3, label="SeqScan")
bi3 = ax3.bar(x3 + w3/2, idx_vals, w3, color=IDX, zorder=3, label="B+Tree Index")
ax3.set_xticks(x3)
ax3.set_xticklabels(labels3, fontsize=9.5)
ymax3 = max(seq_vals) * 1.42
ax3.set_ylim(0, ymax3)
style(ax3, "인덱스 최적화  (ms / query  낮을수록 빠름)", "ms / query", "5,000행 테이블 · 300회 평균")

# SeqScan 위 값
for bar, v in zip(bs3, seq_vals):
    ax3.text(bar.get_x() + bar.get_width()/2, v + ymax3*0.025,
             f"{v:.1f}", ha="center", va="bottom", fontsize=8, color=SUB)
# Index 위 값
for bar, v in zip(bi3, idx_vals):
    ax3.text(bar.get_x() + bar.get_width()/2, v + ymax3*0.025,
             f"{v:.3f}", ha="center", va="bottom", fontsize=8, color="#C2410C")
# 배속 배지
for i, sp in enumerate(speedups):
    ax3.text(x3[i], seq_vals[i] + ymax3 * 0.085,
             f"{sp:.0f}×", ha="center", va="bottom",
             fontsize=12, fontweight="bold", color="#9A3412")

ax3.legend(handles=[
    mpatches.Patch(color=SEQ, label="SeqScan"),
    mpatches.Patch(color=IDX, label="B+Tree Index"),
], fontsize=9, framealpha=0.95, edgecolor=GRID, loc="upper right",
   handlelength=1.0, handleheight=0.9)

# ── ④ 병렬 집계 ──────────────────────────────────────────────────────────
off_v, on_v = par["off_ms"], par["on_ms"]
sp_par = off_v / on_v

bp4 = ax4.bar(0, off_v, W, color=P_OFF, zorder=3)
bn4 = ax4.bar(1, on_v,  W, color=P_ON,  zorder=3)
ax4.set_xticks([0, 1])
ax4.set_xticklabels(["PARALLEL  OFF", "PARALLEL  ON"])
ymax4 = max(off_v, on_v) * 1.3
ax4.set_ylim(0, ymax4)
style(ax4, "병렬 집계  GROUP BY  (ms / query  낮을수록 빠름)", "ms / query",
      f"50,000행 · rayon · {sp_par:.2f}× speedup")
vbar_label(ax4, bp4[0], "{:.1f}", ymax4, " ms", color=SUB)
vbar_label(ax4, bn4[0], "{:.1f}", ymax4, " ms", color=P_ON)

# 절감 화살표
ax4.annotate("", xy=(1, on_v + ymax4*0.06), xytext=(0, off_v + ymax4*0.06),
             arrowprops=dict(arrowstyle="-|>", color=P_ON, lw=1.8,
                             connectionstyle="arc3,rad=-0.15"))
ax4.text(0.5, (off_v + on_v)/2 + ymax4*0.08, f"{sp_par:.2f}×",
         ha="center", va="bottom", fontsize=13, fontweight="bold", color=P_ON,
         transform=ax4.get_xaxis_transform())
badge(ax4, f"parallel chunk + par_iter",
      "#5B21B6", "#F5F3FF", "#DDD6FE")

# ── 저장 & 표시 ───────────────────────────────────────────────────────────
out = HERE / "benchmark_result.png"
plt.savefig(out, dpi=150, bbox_inches="tight", facecolor=BG)
print(f"저장됨: {out}")
plt.show()
