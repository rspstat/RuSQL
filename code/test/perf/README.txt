================================================================
  RuSQL v2.2.0 vs MySQL — 성능 벤치마크 가이드
================================================================

[ 폴더 구조 ]

  perf/
  ├── bench.py          측정 스크립트 (결과 → result.json)
  ├── chart.py          차트 생성 스크립트 (result.json → charts/*.png)
  ├── requirements.txt  Python 의존 패키지
  └── README.txt        이 파일

  실행 후 생성:
  ├── result.json       측정 원본 데이터
  └── charts/
      ├── 01_insert_tps.png
      ├── 02_select_eq.png
      ├── 03_select_range.png
      ├── 04_parallel.png
      └── 05_concurrent.png

----------------------------------------------------------------
[ 사전 준비 ]

1. rustdb-server 실행 — 아래 중 하나
     (a) UI: Server Manager → Start  ← 권장
     (b) 터미널: cargo run -p rustdb-server  (code/ 디렉터리)

2. MySQL 실행 확인
     mysql -u root -e "SELECT VERSION();"

3. Python 패키지 설치
     pip install -r requirements.txt

----------------------------------------------------------------
[ 설정 변경 (bench.py 상단) ]

  RUSTDB_HOST / RUSTDB_PORT  기본: 127.0.0.1 / 7878
  RUSTDB_USER / RUSTDB_PASS  기본: root / root

  MYSQL_HOST / MYSQL_PORT    기본: 127.0.0.1 / 3306
  MYSQL_USER / MYSQL_PASS    기본: root / ""  ← 비밀번호 있으면 수정
  MYSQL_DB                   기본: bench_db   (자동 생성됨)

  N_INSERT   INSERT TPS 측정 행 수  기본: 10,000
  N_SELECT   SELECT 반복 횟수       기본: 1,000

----------------------------------------------------------------
[ 실행 ]

  # code/test/perf/ 디렉터리에서
  python bench.py     # 측정 실행 (약 3~10분 소요)
  python chart.py     # 차트 PNG 생성

----------------------------------------------------------------
[ 측정 항목 ]

  1. INSERT TPS
     - 1만 행 단건 INSERT (auto-commit)
     - RuSQL vs MySQL TPS 비교

  2. SELECT 등호 latency (5,000 rows)
     - SeqScan  : 인덱스 없이 full scan
     - B-tree   : CREATE INDEX ... (일반 인덱스)
     - Hash     : CREATE INDEX ... USING HASH (RuSQL 전용, 등호 O(1))
     - MySQL Hash는 Memory 엔진 전용이므로 B-tree 값으로 표시됨

  3. SELECT 범위 latency (5,000 rows)
     - BETWEEN 조건, 인덱스 없음 vs B-tree 있음
     - RuSQL vs MySQL 비교

  4. 병렬 스케일링 (RuSQL only)
     - RUSTDB_PARALLEL=0 (순차) vs RUSTDB_PARALLEL=1 (병렬)
     - GROUP BY 집계 (COUNT / SUM / AVG) 처리 시간 비교
     - speedup 배수 출력

  5. 동시 접속 SELECT TPS
     - 1 / 4 / 8 스레드 동시 접속
     - 스레드당 500 쿼리 (PK 등호 SELECT)
     - RuSQL vs MySQL 총 TPS 비교

----------------------------------------------------------------
[ 출력 예시 ]

  ============================================================
    RuSQL v2.2.0 vs MySQL — Performance Benchmark
  ============================================================

  [1/5] INSERT TPS (10,000 rows, auto-commit) ...
    INSERT TPS                               RuSQL:    3,200.0 TPS   MySQL:    1,800.0 TPS

  [2/5] SELECT 등호 latency (5,000 rows) ...
    SeqScan                                  RuSQL:        2.5 ms/query   MySQL:        0.3 ms/query
    B-tree Index                             RuSQL:        0.8 ms/query   MySQL:        0.1 ms/query
    Hash Index                               RuSQL:        0.4 ms/query   MySQL:        0.1 ms/query

  ...

  결과 저장: result.json
  차트 생성: python chart.py

----------------------------------------------------------------
[ 실행 환경 기록 — 측정 후 채워주세요 ]

  OS      : Windows 11 Pro
  CPU     :
  RAM     :
  Storage :                    (SSD / HDD)
  RuSQL  : v2.2.0
  MySQL   : 8.0.x
  Python  : 3.x

----------------------------------------------------------------
[ 주의 사항 ]

  - bench.py 실행 중 bench_db 데이터베이스를 생성/삭제합니다.
    기존에 bench_db 가 있다면 데이터가 초기화될 수 있습니다.
  - rustdb-server 가 실행 중이지 않으면 연결 오류가 발생합니다.
  - 병렬 스케일링 측정은 RuSQL 단독 측정이며 MySQL과 비교하지 않습니다.
  - 측정 환경 (CPU 코어 수, RAM, Storage 종류) 에 따라 수치가 달라집니다.
    result.json 에 측정값이 저장되므로 환경 정보를 별도 기록해두세요.

================================================================
