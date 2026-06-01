# RustDB 성능 평가 계획 (v2.2.0)

---

## 1. 목표

"왜 Rust로 RDBMS를 만들었는가"에 대한 수치적 근거를 확보하고,  
B+Tree 인덱스 및 WAL 기반 구조의 실질적 효과를 시각적으로 증명한다.

---

## 2. 비교 대상

| 항목 | RustDB v2.2.0 | MySQL 8.0 |
|------|--------------|-----------|
| 접속 방식 | MySQL wire protocol (포트 3306, mysql_native_password 인증) | 기본 포트 3306 |
| 클라이언트 | mysql-connector-python | mysql-connector-python |
| 실행 환경 | 동일 머신, 단일 프로세스 |

---

## 3. 측정 항목

### 3-1. INSERT TPS (처리량)

| 시나리오 | 건수 | 측정값 |
|----------|------|--------|
| 단건 INSERT × 10,000 | 10,000 | TBD |
| 단건 INSERT × 100,000 | 100,000 | TBD |
| Bulk INSERT (VALUES 100개씩) | 10,000 | TBD |

- 트랜잭션: 매 INSERT마다 auto-commit
- 비교: RustDB vs MySQL

### 3-2. SELECT 성능 (인덱스 유무 비교)

| 시나리오 | 조건 | 인덱스 | 측정값 |
|----------|------|--------|--------|
| Full Scan | WHERE val = ? | ✗ | TBD |
| Index Scan | WHERE val = ? | ✓ (B+Tree) | TBD |
| Range Scan | WHERE val BETWEEN ? AND ? | ✓ | TBD |

- 데이터: 100,000건 사전 적재
- EXPLAIN으로 실행 경로 함께 캡처

### 3-3. 동시 접속 SELECT

| 동시 연결 수 | 총 쿼리 수 | 측정값 |
|-------------|-----------|--------|
| 1 | 1,000 | TBD |
| 4 | 1,000 | TBD |
| 8 | 1,000 | TBD |
| 16 | 1,000 | TBD |

- `threading` 또는 `concurrent.futures` 사용
- RustDB vs MySQL 동시성 처리량 비교

### 3-4. 병렬 쿼리 효과 (rayon, 내부 측정 완료)

| 시나리오 | 순차 (`RUSTDB_PARALLEL=0`) | 병렬 (`=1`) |
|----------|---------------------------|-------------|
| 100,000행 SeqScan + 복합 LIKE WHERE | 0.318 s | 0.261 s (약 18% 단축) |

- 환경변수 `RUSTDB_PARALLEL`로 on/off 토글, 10,000행 이상에서 자동 적용
- 병렬/순차 결과 완전 일치(정확성 검증 완료) — 대형 테이블 풀스캔 필터가 주 수혜
- 측정: release 빌드, WHERE `name LIKE 'row1%' AND v > 100 AND name LIKE '%9%'`

---

## 4. 측정 스크립트 구조

```
mds/
benchmark/
  bench_insert.py      # INSERT TPS 측정
  bench_select.py      # SELECT Full Scan vs Index Scan
  bench_concurrent.py  # 동시 접속 처리량
  bench_all.py         # 전체 실행 + 결과 출력
  requirements.txt     # mysql-connector-python, matplotlib
```

### 핵심 코드 패턴

```python
import mysql.connector, time, statistics

def connect(port=3306):
    return mysql.connector.connect(
        host="127.0.0.1", port=port,
        user="root", password="root",
        database="bench_db"
    )

def measure_insert_tps(port, n=10000):
    conn = connect(port)
    cur = conn.cursor()
    cur.execute("DROP TABLE IF EXISTS bench")
    cur.execute("CREATE TABLE bench (id INT PRIMARY KEY AUTO_INCREMENT, val INT)")
    start = time.time()
    for i in range(n):
        cur.execute("INSERT INTO bench (val) VALUES (%s)", (i,))
        conn.commit()
    elapsed = time.time() - start
    conn.close()
    return n / elapsed  # TPS
```

---

## 5. 결과 시각화

`matplotlib`으로 다음 그래프 생성:

1. **INSERT TPS 막대그래프** — RustDB vs MySQL (10k / 100k)
2. **SELECT 응답시간 비교** — Full Scan vs Index Scan (ms)
3. **동시 접속 처리량 선그래프** — 연결 수에 따른 TPS 변화

```python
import matplotlib.pyplot as plt

labels = ['RustDB', 'MySQL']
tps = [rustdb_tps, mysql_tps]

plt.bar(labels, tps, color=['#4ec9b0', '#e06c75'])
plt.title('INSERT TPS (10,000 rows)')
plt.ylabel('Transactions per Second')
plt.savefig('insert_tps.png', dpi=150)
plt.show()
```

---

## 6. 실행 환경 기록

| 항목 | 내용 |
|------|------|
| OS | Windows 11 Pro |
| CPU | TBD |
| RAM | TBD |
| Storage | TBD (SSD / HDD) |
| RustDB 버전 | v2.2.0 |
| MySQL 버전 | 8.0.x |
| Python 버전 | 3.x |

---

## 7. 진행 체크리스트

- [ ] benchmark/ 폴더 및 스크립트 작성
- [ ] 더미 데이터 생성 (faker 활용, 100,000건)
- [ ] RustDB INSERT TPS 측정
- [ ] MySQL INSERT TPS 측정
- [ ] RustDB SELECT Full Scan / Index Scan 측정
- [ ] MySQL SELECT Full Scan / Index Scan 측정
- [ ] 동시 접속 처리량 측정 (RustDB / MySQL)
- [ ] 결과 그래프 생성 (matplotlib)
- [ ] README 또는 발표 자료에 수치 반영
