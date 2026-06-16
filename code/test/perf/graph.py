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
tx  = r["transaction"]

ins_s = s["rows"]  / s["insert_s"]
del_s = s["rows"]  / s["delete_s"]
ins_b = b["rows"]  / b["insert_s"]
del_b = b["rows"]  / b["delete_s"]
auto_tps = tx["rows"] / tx["auto_s"]
txn_tps  = tx["rows"] / tx["txn_s"]

# ── 색상 ─────────────────────────────────────────────────────────────────
INS   = "#3B82F6"   # blue
DEL   = "#10B981"   # emerald
SEQ   = "#94A3B8"   # slate
IDX   = "#F97316"   # orange
TXN   = "#F59E0B"   # amber
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
style(ax1, f"단건 쓰기  ({s['rows']:,} rows)", "TPS",
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
style(ax2, f"Bulk 쓰기  ({b['rows']:,} rows)", "TPS",
      f"{b['rows']:,}건 · 500행 묶음")
vbar_label(ax2, bi2[0], "{:,.0f}", ymax2, color=INS)
vbar_label(ax2, bd2[0], "{:,.0f}", ymax2, color=DEL)

# ── ③ 인덱스 성능 (포인트 조회) ───────────────────────────────────────────
seq_tps = 1000 / pl["seq_ms"] if pl["seq_ms"] > 0 else 0
idx_tps = 1000 / pl["idx_ms"] if pl["idx_ms"] > 0 else 0
sp      = pl["speedup"]

bs3 = ax3.bar(0, seq_tps, W, color=SEQ, zorder=3)
bi3 = ax3.bar(1, idx_tps, W, color=IDX, zorder=3)
ax3.set_xticks([0, 1])
ax3.set_xticklabels(["SeqScan", "B+Tree Index"])
ymax3 = max(seq_tps, idx_tps) * 1.40
ax3.set_ylim(0, ymax3)
ax3.yaxis.set_major_formatter(plt.FuncFormatter(lambda x, _: f"{x:,.0f}"))
style(ax3, "인덱스 성능  (TPS  높을수록 빠름)", "TPS",
      "5,000행 · 포인트 조회 · 300회 평균")
vbar_label(ax3, bs3[0], "{:,.0f}", ymax3, color=SUB)
vbar_label(ax3, bi3[0], "{:,.0f}", ymax3, color="#C2410C")
ax3.text(0.5, ymax3 * 0.55, f"{sp:.0f}×",
         ha="center", va="bottom", fontsize=16, fontweight="bold", color="#9A3412",
         transform=ax3.get_xaxis_transform())

# ── ④ 트랜잭션 TPS ───────────────────────────────────────────────────────
sp_tx = auto_tps / txn_tps if txn_tps > 0 else 0

ba4 = ax4.bar(0, auto_tps, W, color=INS, zorder=3)
bt4 = ax4.bar(1, txn_tps,  W, color=TXN, zorder=3)
ax4.set_xticks([0, 1])
ax4.set_xticklabels(["AutoCommit\n(묵시적)", "BEGIN/COMMIT\n(명시적)"])
ymax4 = max(auto_tps, txn_tps) * 1.40
ax4.set_ylim(0, ymax4)
ax4.yaxis.set_major_formatter(plt.FuncFormatter(lambda x, _: f"{x:,.0f}"))
style(ax4, "트랜잭션 TPS  (높을수록 빠름)", "TPS",
      f"{tx['rows']:,}건 · AutoCommit {sp_tx:.0f}× 빠름")
vbar_label(ax4, ba4[0], "{:,.0f}", ymax4, color=INS)
vbar_label(ax4, bt4[0], "{:,.0f}", ymax4, color=TXN)
ax4.text(0.5, ymax4 * 0.55, f"{sp_tx:.0f}×",
         ha="center", va="bottom", fontsize=16, fontweight="bold", color=TXN,
         transform=ax4.get_xaxis_transform())

# ── 저장 & 표시 ───────────────────────────────────────────────────────────
out = HERE / "benchmark_result.png"
plt.savefig(out, dpi=150, bbox_inches="tight", facecolor=BG)
print(f"저장됨: {out}")
plt.show()
