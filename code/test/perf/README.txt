================================================================
  RuSQL 성능 벤치마크 가이드
================================================================

[ 폴더 구조 ]

  perf/
  ├── bench.py          RuSQL 측정 스크립트 (결과 → result.json, UI의 Benchmark 패널이 읽음)
  ├── bench_mysql.py    같은 4개 항목을 MySQL에서 측정 (참고용, 결과 → result_mysql.json)
  ├── bench_query.py    5만 행에서 스캔·집계·GROUP BY·DISTINCT·ORDER BY·조인 15종의 중앙값(ms), 결과 → result_query.json
                        (UI 패널용이 아니라 두 빌드 비교용; 40초 넘는 질의는 timeout으로 표시하고 이후 질의는 건너뜀)
  ├── graph.py          발표용 그래프 (result.json → benchmark_result.png)
  ├── chart.py          예전 형식(RuSQL vs MySQL 5개 항목)용 차트 스크립트 -- 현재 result.json과는 형식이 다름
  ├── requirements.txt  Python 의존 패키지
  └── README.txt        이 파일

  목표는 MySQL을 따라잡는 것이 아니라 RuSQL 자신의 최적화를 전후로 비교하는 것이다.
  MySQL 수치는 맥락을 잡기 위한 참고값이다.

----------------------------------------------------------------
[ 측정 항목 (bench.py) ]

  1. 단건 INSERT/DELETE 10,000건 (autocommit, PK 등호 DELETE)
  2. Bulk INSERT/DELETE 100,000건 (500행 묶음, DELETE는 PK 범위)
  3. 포인트 조회 (5,000행, 300회): 인덱스 없음 vs 보조 B+Tree 인덱스
  4. 트랜잭션: AutoCommit 1,000건 / 건당 BEGIN·INSERT·COMMIT 1,000건 /
     하나의 트랜잭션에 INSERT 1,000건

----------------------------------------------------------------
[ 실행 ]

  1. RuSQL 서버 실행 (7878 포트)
       code/build/backend/server/Release/engine_server.exe --port 7878 --no-mysql --data-dir <빈 디렉터리>
     (UI의 Server Manager에서 시작해도 됨)

  2. code/test/perf/ 에서
       python bench.py        # 약 1분, result.json 생성
       python graph.py        # benchmark_result.png 생성 (MPLBACKEND=Agg 로 창 없이)

  같은 PC에서도 백그라운드 작업(실시간 검사, 빌드 등)에 따라 값이 크게 흔들린다(건당 트랜잭션이 같은 빌드에서
  7~11초). 전후 비교는 같은 상태에서 번갈아 여러 번(5회 이상) 재고 중앙값을 쓴다.

----------------------------------------------------------------
[ MySQL 참고 측정 (bench_mysql.py) ]

  RuSQL의 autocommit은 문장마다 redo 로그를 fsync(내구성)하므로, MySQL도 같은 내구성 조건으로 돌린다:
  InnoDB 기본값 innodb_flush_log_at_trx_commit=1 (커밋마다 redo fsync), 바이너리 로그는 끈다(--skip-log-bin;
  켜 두면 커밋마다 fsync가 한 번 더 생겨 RuSQL보다 불리해진다).

    mysqld --no-defaults --datadir=<빈 디렉터리> --port=3307 --mysqlx=OFF --skip-log-bin --innodb-flush-log-at-trx-commit=1
    python bench_mysql.py 3307           # PyMySQL 필요, result_mysql.json 생성

  (처음이면 mysqld --no-defaults --initialize-insecure --datadir=<빈 디렉터리> 로 데이터 디렉터리를 먼저 만든다.)

----------------------------------------------------------------
[ 주의 사항 ]

  - bench.py / bench_mysql.py 는 bench_db 데이터베이스를 만들고 지운다. 기존 bench_db 가 있으면 사라진다.
  - 서버가 실행 중이지 않으면 연결 오류가 난다.
  - 수치는 CPU, 디스크(fsync 지연), 백신 실시간 검사에 크게 좌우된다. 최신 측정값과 해석은
    docs/mds/DATE.md 의 10월 2일 항목을 참고.
