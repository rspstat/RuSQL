# archive

지금 제품과 개발에는 쓰지 않지만 **작업 기록으로 남겨 두는 것**들입니다. 빌드·테스트·MCP·앱 어디에서도 이 폴더를 참조하지 않습니다.

| 폴더 | 내용 | 왜 보관하나 |
|---|---|---|
| `AI/` | 자연어→SQL 파인튜닝 실험: 합성 데이터 생성기(`data_generation/`, 3,718행 데이터셋 포함)와 Colab 노트북(`finetuning/`, Qwen2.5-Coder-1.5B + LoRA, held-out 스키마 평가 93.7%) | 2026-10-01에 사설 모델을 제품에서 뺐다(AI는 Claude + MCP로 일원화). 실험 결과 자체는 졸업작품 기록으로 남김. 상세: `docs/mds/AI.md` |
| `docs/` | 1학기 때의 통합 문서(docx)와 옛 다이어그램 | 현재 문서는 `docs/mds/`, `docs/2026_졸업작품_산출물_개정/` |
| `code/test/perf/chart.py` | 예전 형식(RuSQL vs MySQL 5개 항목)용 차트 스크립트 | 지금의 `result.json` 형식과 맞지 않음. 현재는 `graph.py`, `speedup_chart.py` 사용 |

1학기 Rust 원본 소스는 폴더가 아니라 Git의 `legacy` 브랜치에 있습니다(`git checkout legacy`).
