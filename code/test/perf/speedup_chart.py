"""
RuSQL 성능 개선 배율 그래프 (발표용, 라이트 모드)
  - 2학기 후반의 성능 작업(커밋 944b2f3 ~ 145b5d8)에서 "같은 PC, 같은 상태에서 이전 빌드와 번갈아" 잰 쌍의 배율만 사용한다.
    서로 다른 날의 절댓값을 섞지 않기 위해 시간이 아니라 배율(이전 / 이후)만 그린다.
  - 출처와 측정 조건: docs/mds/DATE.md 의 10월 2일·3일 항목
  - 실행: MPLBACKEND=Agg python speedup_chart.py   → speedup_chart.png
"""

from pathlib import Path

import matplotlib.pyplot as plt

HERE = Path(__file__).parent

# (라벨, 이전, 이후, 단위)  -- 이전/이후는 같은 상태에서 번갈아 잰 중앙값
GROUPS = [
    ("쓰기", [
        ("명시적 트랜잭션 커밋 (건당 BEGIN·COMMIT)", 7.47, 0.92, "초"),
        ("Bulk INSERT 100,000건", 2.36, 1.04, "초"),
        ("Bulk DELETE 100,000건", 1.21, 0.72, "초"),
        ("전체 행 UPDATE 100,000건", 4.0, 2.2, "초"),
    ]),
    ("조회·집계 (5만 행)", [
        ("ORDER BY … LIMIT 10", 446, 40, "ms"),
        ("GROUP BY", 189, 34.3, "ms"),
        ("COUNT(*)", 87.7, 18.2, "ms"),
        ("DISTINCT", 104, 31.5, "ms"),
        ("인덱스 없는 필터 조회", 91, 23, "ms"),
    ]),
    ("조인", [
        ("선택적 조인 (5만 × 2천 행)", 310, 38.5, "ms"),
        ("LEFT JOIN (5천 × 2천 행)", 14211, 6.5, "ms"),
        ("RIGHT JOIN (2천 × 2천 행)", 6505, 42, "ms"),
        ("FULL OUTER JOIN (2천 × 2천 행)", 6511, 37, "ms"),
        ("ON 조건이 둘인 JOIN (5천 행)", 13494, 55, "ms"),
    ]),
    ("서브쿼리", [
        ("WHERE 안 스칼라 서브쿼리 (5천 행)", 3693, 2.9, "ms"),
        ("상관 EXISTS, 인덱스 없는 FK (3천 × 3천 행)", 3563, 46, "ms"),
        ("서브쿼리가 든 UPDATE (5천 행)", 7846, 62, "ms"),
    ]),
]

BG, GRID, TEXT, SUB = "#FFFFFF", "#E2E8F0", "#0F172A", "#64748B"
COLORS = {"쓰기": "#3B82F6", "조회·집계 (5만 행)": "#10B981", "조인": "#F97316", "서브쿼리": "#8B5CF6"}

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Malgun Gothic", "AppleGothic", "NanumGothic", "Arial Unicode MS", "DejaVu Sans"],
})

rows = []
for group, items in GROUPS:
    for label, before, after, unit in items:
        rows.append((group, label, before / after, before, after, unit))
rows.reverse()  # barh draws the first row at the bottom

fig, ax = plt.subplots(figsize=(13, 9.5), facecolor=BG)
ax.set_facecolor(BG)
for i, (group, label, ratio, before, after, unit) in enumerate(rows):
    ax.barh(i, ratio, color=COLORS[group], height=0.62, zorder=3)
    ax.text(ratio * 1.12, i, f"{ratio:,.1f}×   ({before:,g} → {after:,g} {unit})" if ratio < 10 else f"{ratio:,.0f}×   ({before:,g} → {after:,g} {unit})",
            va="center", fontsize=9.5, color=TEXT)
ax.set_yticks(range(len(rows)))
ax.set_yticklabels([r[1] for r in rows], fontsize=10.5, color=TEXT)
ax.set_xscale("log")
ax.set_xlim(1, 2e5)
ax.xaxis.set_major_formatter(plt.FuncFormatter(lambda x, _: f"{x:,.0f}×"))
ax.tick_params(axis="x", colors=SUB, labelsize=10)
ax.tick_params(axis="y", length=0)
ax.xaxis.grid(True, color=GRID, linewidth=1.1, zorder=0)
ax.set_axisbelow(True)
ax.spines[["top", "right", "left"]].set_visible(False)
ax.spines["bottom"].set_color(GRID)
ax.set_xlabel("개선 배율 (이전 ÷ 이후, 로그 눈금)", fontsize=10.5, color=SUB, labelpad=8)

handles = [plt.Rectangle((0, 0), 1, 1, color=c) for c in COLORS.values()]
ax.legend(handles, COLORS.keys(), loc="lower right", frameon=False, fontsize=10.5, labelcolor=TEXT)
fig.text(0.5, 0.975, "RuSQL  ·  성능 개선 배율", ha="center", va="top", fontsize=22, fontweight="bold", color=TEXT)
fig.text(0.5, 0.935, "같은 PC·같은 상태에서 이전 빌드와 번갈아 잰 쌍 · 결과는 모든 질의에서 이전과 동일(차분 검증)",
         ha="center", va="top", fontsize=10.5, color=SUB)
fig.subplots_adjust(left=0.30, right=0.97, top=0.89, bottom=0.08)

out = HERE / "speedup_chart.png"
plt.savefig(out, dpi=150, bbox_inches="tight", facecolor=BG)
print(f"저장됨: {out}")
