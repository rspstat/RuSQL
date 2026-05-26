"""
RustDB MCP Server
Natural language → SQL, EXPLAIN interpretation via Gemini API
Run: uvicorn server:app --host 127.0.0.1 --port 8765
"""
import re
from google import genai
from google.genai import types
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

MODEL = "gemini-2.5-flash"


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


class ChatTurn(BaseModel):
    role: str
    content: str


class OpenFile(BaseModel):
    name: str
    content: str


class ChatRequest(BaseModel):
    messages: list[ChatTurn]
    schema: str
    api_key: str
    current_db: str = "rustdb"
    open_files: list[OpenFile] = []


def strip_fences(text: str) -> str:
    if text.startswith("```"):
        lines = text.split("\n")
        end = -1 if lines[-1].startswith("```") else len(lines)
        return "\n".join(lines[1:end]).strip()
    return text


def handle_error(e: Exception):
    msg = str(e)
    if "API_KEY_INVALID" in msg or "API key" in msg or "invalid" in msg.lower():
        raise HTTPException(status_code=401, detail="Invalid API key")
    raise HTTPException(status_code=500, detail=msg)


@app.get("/health")
def health():
    return {"status": "ok", "model": MODEL}


@app.post("/api/nl-to-sql")
def nl_to_sql(req: NLToSQLRequest):
    if not req.api_key.strip():
        raise HTTPException(status_code=400, detail="API key is required")
    if not req.question.strip():
        raise HTTPException(status_code=400, detail="Question is required")

    system = (
        f"You are a SQL expert for RustDB (MySQL-compatible syntax). "
        f"Current database: {req.current_db}\n\nDatabase schema:\n{req.schema}\n\n"
        "Return ONLY the raw SQL query with no markdown fences, no explanation."
    )
    try:
        client = genai.Client(api_key=req.api_key)
        response = client.models.generate_content(
            model=MODEL,
            contents=f"Request: {req.question}",
            config=types.GenerateContentConfig(system_instruction=system),
        )
        sql = strip_fences(response.text.strip())
        return {"sql": sql}
    except Exception as e:
        handle_error(e)


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
        client = genai.Client(api_key=req.api_key)
        response = client.models.generate_content(model=MODEL, contents=prompt)
        return {"interpretation": response.text.strip()}
    except Exception as e:
        handle_error(e)


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
        client = genai.Client(api_key=req.api_key)
        response = client.models.generate_content(model=MODEL, contents=prompt)
        sql = strip_fences(response.text.strip())
        return {"sql": sql}
    except Exception as e:
        handle_error(e)


@app.post("/api/chat")
def chat(req: ChatRequest):
    if not req.api_key.strip():
        raise HTTPException(status_code=400, detail="API key is required")

    files_ctx = ""
    if req.open_files:
        files_ctx = "\n\nOpen SQL files in the editor:\n"
        for f in req.open_files:
            files_ctx += f"\n--- {f.name} ---\n```sql\n{f.content}\n```\n"

    system = f"""You are an AI assistant for RustDB, a MySQL-compatible SQL database engine.
Current database: {req.current_db}

Database schema:
{req.schema}{files_ctx}
Help the user with SQL queries, query optimization, and database design.
Respond in Korean when the user writes in Korean.

When generating a standalone SQL query (not a file edit), place it at the very end of your response inside a ```sql ... ``` code block.

When the user asks you to MODIFY, INSERT INTO, or DELETE FROM a file:
1. Briefly explain what you changed (1-2 sentences).
2. Return the COMPLETE modified file content using this exact format (no extra text inside the block):
<<<FILE filename.sql
[complete modified file content here]
FILE>>>
You may return multiple FILE blocks if editing multiple files.
Do NOT use a ```sql block when returning a file edit."""

    try:
        client = genai.Client(api_key=req.api_key)

        history = [
            types.Content(
                role="user" if m.role == "user" else "model",
                parts=[types.Part(text=m.content)],
            )
            for m in req.messages[:-1]
        ]
        chat_session = client.chats.create(
            model=MODEL,
            config=types.GenerateContentConfig(system_instruction=system),
            history=history,
        )
        response = chat_session.send_message(req.messages[-1].content)
        full = response.text.strip()

        # 1. Extract <<<FILE ... FILE>>> blocks
        file_pattern = re.compile(r'<<<FILE (.+?)\n(.*?)FILE>>>', re.DOTALL)
        file_edits = [
            {"name": m.group(1).strip(), "content": m.group(2).rstrip()}
            for m in file_pattern.finditer(full)
        ]
        cleaned = file_pattern.sub('', full).strip()

        # 2. Extract SQL block from remaining text
        sql = None
        content = cleaned
        if "```sql" in cleaned:
            before, rest = cleaned.split("```sql", 1)
            sql = rest.split("```")[0].strip()
            content = before.strip() or "SQL을 생성했습니다."
        elif "```" in cleaned:
            parts = cleaned.split("```")
            candidate = parts[1].strip()
            if any(kw in candidate.upper() for kw in ("SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "ALTER")):
                sql = candidate
                content = parts[0].strip() or "SQL을 생성했습니다."

        if not content and file_edits:
            content = "파일을 수정했습니다."

        return {"content": content, "sql": sql, "file_edits": file_edits or None}
    except Exception as e:
        handle_error(e)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8765, log_level="info")
