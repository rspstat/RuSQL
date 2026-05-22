"""
RustDB MCP Server
Natural language → SQL, EXPLAIN interpretation via Claude API
Run: uvicorn server:app --host 127.0.0.1 --port 8765
"""
import anthropic
from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel

app = FastAPI(title="RustDB MCP Server", version="1.0.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

MODEL = "claude-opus-4-7"


class NLToSQLRequest(BaseModel):
    question: str
    schema: str
    api_key: str
    current_db: str = "rustdb"


class ExplainRequest(BaseModel):
    sql: str
    explain_result: str
    api_key: str


class SchemaDesignRequest(BaseModel):
    description: str
    api_key: str


def make_client(api_key: str) -> anthropic.Anthropic:
    return anthropic.Anthropic(api_key=api_key)


@app.get("/health")
def health():
    return {"status": "ok", "model": MODEL}


@app.post("/api/nl-to-sql")
def nl_to_sql(req: NLToSQLRequest):
    if not req.api_key.strip():
        raise HTTPException(status_code=400, detail="API key is required")
    if not req.question.strip():
        raise HTTPException(status_code=400, detail="Question is required")

    prompt = f"""You are a SQL expert for the RustDB database engine (MySQL-compatible syntax).
Current database: {req.current_db}

Database schema:
{req.schema}

Convert the following natural language request into a SQL query.
Return ONLY the raw SQL query with no markdown fences, no explanation, no extra text.

Request: {req.question}"""

    try:
        client = make_client(req.api_key)
        response = client.messages.create(
            model=MODEL,
            max_tokens=1024,
            messages=[{"role": "user", "content": prompt}],
        )
        sql = response.content[0].text.strip()
        # Strip markdown code fences if model added them
        if sql.startswith("```"):
            lines = sql.split("\n")
            sql = "\n".join(lines[1:-1] if lines[-1].startswith("```") else lines[1:])
        return {"sql": sql}
    except anthropic.AuthenticationError:
        raise HTTPException(status_code=401, detail="Invalid API key")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/api/explain")
def explain_query(req: ExplainRequest):
    if not req.api_key.strip():
        raise HTTPException(status_code=400, detail="API key is required")

    prompt = f"""당신은 데이터베이스 성능 전문가입니다. 다음 SQL 쿼리의 실행 계획을 분석하세요.

SQL 쿼리:
{req.sql}

EXPLAIN 결과:
{req.explain_result}

다음 항목을 한국어로 간결하게 설명하세요:
1. 실행 계획 요약 (어떤 방식으로 데이터를 읽는지)
2. 성능 문제가 있다면 구체적으로 설명
3. 개선 방안 (인덱스 생성 등 구체적인 SQL 포함)

최대 5-6문장으로 간결하게 답변하세요."""

    try:
        client = make_client(req.api_key)
        response = client.messages.create(
            model=MODEL,
            max_tokens=1024,
            messages=[{"role": "user", "content": prompt}],
        )
        return {"interpretation": response.content[0].text.strip()}
    except anthropic.AuthenticationError:
        raise HTTPException(status_code=401, detail="Invalid API key")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/api/schema-design")
def schema_design(req: SchemaDesignRequest):
    if not req.api_key.strip():
        raise HTTPException(status_code=400, detail="API key is required")

    prompt = f"""당신은 데이터베이스 설계 전문가입니다. RustDB (MySQL 호환 문법)를 사용합니다.

다음 시스템 요구사항에 맞는 테이블 구조를 설계하고 CREATE TABLE SQL을 작성하세요.
적절한 기본키, 외래키, 인덱스를 포함하세요.

요구사항: {req.description}

CREATE TABLE SQL 구문만 반환하세요 (마크다운 코드 블록 없이)."""

    try:
        client = make_client(req.api_key)
        response = client.messages.create(
            model=MODEL,
            max_tokens=2048,
            messages=[{"role": "user", "content": prompt}],
        )
        sql = response.content[0].text.strip()
        if sql.startswith("```"):
            lines = sql.split("\n")
            sql = "\n".join(lines[1:-1] if lines[-1].startswith("```") else lines[1:])
        return {"sql": sql}
    except anthropic.AuthenticationError:
        raise HTTPException(status_code=401, detail="Invalid API key")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8765, log_level="info")
