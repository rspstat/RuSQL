// MySQL wire protocol (text protocol, MySQL 4.1+)
// Auth is not verified — connections are accepted in dev mode.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, RwLock};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::thread;

use rustdb_core::parser::parser::Parser;
use rustdb_core::engine::executor::{Executor, SharedDatabase};

// COM command bytes
const COM_QUIT:         u8 = 0x01;
const COM_INIT_DB:      u8 = 0x02;
const COM_QUERY:        u8 = 0x03;
const COM_PING:         u8 = 0x0e;
const COM_STMT_PREPARE: u8 = 0x16;
const COM_STMT_EXECUTE: u8 = 0x17;
const COM_STMT_CLOSE:   u8 = 0x19;
const COM_STMT_RESET:   u8 = 0x1a;

// Server capability flags advertised to clients
const CAPS: u32 =
    0x0001 |       // CLIENT_LONG_PASSWORD
    0x0004 |       // CLIENT_LONG_FLAG
    0x0200 |       // CLIENT_PROTOCOL_41
    0x2000 |       // CLIENT_TRANSACTIONS
    0x8000 |       // CLIENT_SECURE_CONNECTION
    0x0008_0000 |  // CLIENT_PLUGIN_AUTH
    0x0001_0000 |  // CLIENT_MULTI_STATEMENTS
    0x0004_0000;   // CLIENT_MULTI_RESULTS

// Prepared statement entry
struct PreparedStmt {
    query:      String,
    num_params: usize,
}

type StmtMap = std::collections::HashMap<u32, PreparedStmt>;

// ── Packet I/O ─────────────────────────────────────────────────

fn read_packet(s: &mut TcpStream) -> Option<(u8, Vec<u8>)> {
    let mut h = [0u8; 4];
    s.read_exact(&mut h).ok()?;
    let len = (h[0] as usize) | ((h[1] as usize) << 8) | ((h[2] as usize) << 16);
    let seq = h[3];
    let mut buf = vec![0u8; len];
    s.read_exact(&mut buf).ok()?;
    Some((seq, buf))
}

fn write_packet(s: &mut TcpStream, seq: u8, payload: &[u8]) -> std::io::Result<()> {
    let n = payload.len();
    s.write_all(&[
        (n & 0xff) as u8,
        ((n >> 8) & 0xff) as u8,
        ((n >> 16) & 0xff) as u8,
        seq,
    ])?;
    s.write_all(payload)?;
    s.flush()
}

// ── Length-encoded encoding ─────────────────────────────────────

fn lenenc(buf: &mut Vec<u8>, n: u64) {
    if n < 251 {
        buf.push(n as u8);
    } else if n <= 0xffff {
        buf.push(0xfc);
        buf.push((n & 0xff) as u8);
        buf.push(((n >> 8) & 0xff) as u8);
    } else if n <= 0xffffff {
        buf.push(0xfd);
        buf.push((n & 0xff) as u8);
        buf.push(((n >> 8) & 0xff) as u8);
        buf.push(((n >> 16) & 0xff) as u8);
    } else {
        buf.push(0xfe);
        buf.extend_from_slice(&n.to_le_bytes());
    }
}

fn lenstr(buf: &mut Vec<u8>, s: &[u8]) {
    lenenc(buf, s.len() as u64);
    buf.extend_from_slice(s);
}

fn nulstr(buf: &mut Vec<u8>, s: &str) {
    buf.extend_from_slice(s.as_bytes());
    buf.push(0);
}

// ── Packet builders ────────────────────────────────────────────

fn handshake_pkt(conn_id: u32) -> Vec<u8> {
    // Fixed 20-byte nonce (auth not verified, so content doesn't matter)
    let nonce = b"RUSTDBNONCE123456789\0";
    let mut p = Vec::new();
    p.push(10u8);                                           // protocol version 10
    nulstr(&mut p, "5.7.0-rustdb");                        // server version
    p.extend_from_slice(&conn_id.to_le_bytes());           // connection ID
    p.extend_from_slice(&nonce[..8]);                      // auth-data part-1
    p.push(0x00);                                          // filler
    p.extend_from_slice(&(CAPS as u16).to_le_bytes());     // capability flags (lo)
    p.push(0x21);                                          // charset: utf8
    p.extend_from_slice(&0x0002u16.to_le_bytes());         // status: AUTOCOMMIT
    p.extend_from_slice(&((CAPS >> 16) as u16).to_le_bytes()); // capability flags (hi)
    p.push(21);                                            // auth-plugin-data total len
    p.extend_from_slice(&[0u8; 10]);                       // reserved
    p.extend_from_slice(&nonce[8..20]);                    // auth-data part-2 (12 bytes)
    p.push(0x00);                                          // null padding
    nulstr(&mut p, "mysql_native_password");
    p
}

fn ok_pkt(affected: u64) -> Vec<u8> {
    let mut p = vec![0x00];
    lenenc(&mut p, affected);
    lenenc(&mut p, 0);                                     // last_insert_id
    p.extend_from_slice(&0x0002u16.to_le_bytes());         // status: AUTOCOMMIT
    p.extend_from_slice(&0u16.to_le_bytes());              // warnings
    p
}

fn err_pkt(msg: &str) -> Vec<u8> {
    let mut p = vec![0xff];
    p.extend_from_slice(&1064u16.to_le_bytes());           // ER_PARSE_ERROR
    p.push(b'#');
    p.extend_from_slice(b"42000");                         // SQL state
    p.extend_from_slice(msg.as_bytes());
    p
}

fn eof_pkt() -> Vec<u8> {
    vec![0xfe, 0x00, 0x00, 0x02, 0x00]
}

fn col_def_pkt(name: &str) -> Vec<u8> {
    let mut p = Vec::new();
    lenstr(&mut p, b"def");                                // catalog
    lenstr(&mut p, b"");                                   // schema
    lenstr(&mut p, b"");                                   // table
    lenstr(&mut p, b"");                                   // org_table
    lenstr(&mut p, name.as_bytes());                       // name
    lenstr(&mut p, name.as_bytes());                       // org_name
    p.push(0x0c);                                          // fixed-length fields length
    p.extend_from_slice(&0x0021u16.to_le_bytes());         // charset: utf8 (33)
    p.extend_from_slice(&0xffff_ffffu32.to_le_bytes());    // max column length
    p.push(0xfd);                                          // type: VARSTRING
    p.extend_from_slice(&0u16.to_le_bytes());              // flags
    p.push(0x00);                                          // decimals
    p.extend_from_slice(&0u16.to_le_bytes());              // filler
    p
}

// ── Prepared statement support ─────────────────────────────────

fn stmt_prepare_ok(stmt_id: u32, num_params: u16, num_cols: u16) -> Vec<u8> {
    let mut p = vec![0x00];
    p.extend_from_slice(&stmt_id.to_le_bytes());
    p.extend_from_slice(&num_cols.to_le_bytes());
    p.extend_from_slice(&num_params.to_le_bytes());
    p.push(0x00);                                  // reserved
    p.extend_from_slice(&0u16.to_le_bytes());      // warning_count
    p
}

/// Count `?` placeholders in a SQL string (ignoring those inside quoted strings).
fn count_placeholders(q: &str) -> usize {
    let mut n = 0;
    let mut in_str = false;
    let mut prev = ' ';
    for c in q.chars() {
        if c == '\'' && prev != '\\' { in_str = !in_str; }
        if c == '?' && !in_str { n += 1; }
        prev = c;
    }
    n
}

/// Substitute `?` placeholders with the bound parameter values.
fn bind_params(query: &str, params: &[String]) -> String {
    let mut result = String::with_capacity(query.len() + params.iter().map(|p| p.len()).sum::<usize>());
    let mut param_idx = 0;
    let mut in_str = false;
    let mut prev = ' ';
    for c in query.chars() {
        if c == '\'' && prev != '\\' { in_str = !in_str; }
        if c == '?' && !in_str {
            if let Some(val) = params.get(param_idx) {
                result.push_str(val);
            } else {
                result.push('?');
            }
            param_idx += 1;
        } else {
            result.push(c);
        }
        prev = c;
    }
    result
}

/// Read COM_STMT_EXECUTE payload and return the bound parameter values as SQL literals.
fn parse_execute_params(payload: &[u8], num_params: usize) -> Vec<String> {
    if num_params == 0 { return vec![]; }

    // payload: [stmt_id:4][flags:1][iteration-count:4]
    // = 9 bytes fixed header; then null_bitmap, new_params_bound_flag
    let mut pos = 9; // skip stmt_id(4) + flags(1) + iteration_count(4)
    if pos >= payload.len() { return vec!["NULL".to_string(); num_params]; }

    let null_bitmap_len = (num_params + 7) / 8;
    if pos + null_bitmap_len > payload.len() { return vec!["NULL".to_string(); num_params]; }
    let null_bitmap = &payload[pos..pos + null_bitmap_len];
    pos += null_bitmap_len;

    let new_params_bound = payload.get(pos).copied().unwrap_or(0);
    pos += 1;

    // Read type array if new_params_bound == 1
    let mut types: Vec<(u8, u8)> = Vec::new(); // (field_type, unsigned_flag)
    if new_params_bound == 1 {
        for _ in 0..num_params {
            let ft = payload.get(pos).copied().unwrap_or(0xfd);
            let uf = payload.get(pos + 1).copied().unwrap_or(0);
            types.push((ft, uf));
            pos += 2;
        }
    } else {
        types = vec![(0xfd, 0); num_params]; // default to VAR_STRING
    }

    let mut params = Vec::with_capacity(num_params);
    for i in 0..num_params {
        // Check null bitmap
        let is_null = (null_bitmap[i / 8] >> (i % 8)) & 1 == 1;
        if is_null {
            params.push("NULL".to_string());
            continue;
        }
        let (ft, _unsigned) = types[i];
        let val = match ft {
            0x01 => { // TINY (1 byte)
                let v = payload.get(pos).copied().unwrap_or(0) as i8;
                pos += 1;
                v.to_string()
            }
            0x02 => { // SHORT (2 bytes)
                if pos + 2 > payload.len() { pos = payload.len(); params.push("NULL".to_string()); continue; }
                let v = i16::from_le_bytes([payload[pos], payload[pos+1]]);
                pos += 2;
                v.to_string()
            }
            0x03 | 0x09 => { // LONG (4 bytes)
                if pos + 4 > payload.len() { pos = payload.len(); params.push("NULL".to_string()); continue; }
                let v = i32::from_le_bytes([payload[pos], payload[pos+1], payload[pos+2], payload[pos+3]]);
                pos += 4;
                v.to_string()
            }
            0x08 | 0x10 => { // LONGLONG (8 bytes)
                if pos + 8 > payload.len() { pos = payload.len(); params.push("NULL".to_string()); continue; }
                let b: [u8; 8] = payload[pos..pos+8].try_into().unwrap_or([0;8]);
                let v = i64::from_le_bytes(b);
                pos += 8;
                v.to_string()
            }
            0x04 => { // FLOAT (4 bytes)
                if pos + 4 > payload.len() { pos = payload.len(); params.push("NULL".to_string()); continue; }
                let b: [u8; 4] = payload[pos..pos+4].try_into().unwrap_or([0;4]);
                let v = f32::from_le_bytes(b);
                pos += 4;
                format!("{}", v)
            }
            0x05 => { // DOUBLE (8 bytes)
                if pos + 8 > payload.len() { pos = payload.len(); params.push("NULL".to_string()); continue; }
                let b: [u8; 8] = payload[pos..pos+8].try_into().unwrap_or([0;8]);
                let v = f64::from_le_bytes(b);
                pos += 8;
                format!("{}", v)
            }
            0x0a => { // DATE (4 bytes: year(2), month(1), day(1))
                if pos + 4 > payload.len() { pos = payload.len(); params.push("NULL".to_string()); continue; }
                let y = u16::from_le_bytes([payload[pos], payload[pos+1]]);
                let mo = payload[pos+2];
                let d = payload[pos+3];
                pos += 4;
                format!("'{:04}-{:02}-{:02}'", y, mo, d)
            }
            0x0b | 0x0c => { // DATETIME / TIMESTAMP
                let dlen = payload.get(pos).copied().unwrap_or(0) as usize;
                pos += 1;
                if dlen >= 4 && pos + 4 <= payload.len() {
                    let y = u16::from_le_bytes([payload[pos], payload[pos+1]]);
                    let mo = payload[pos+2];
                    let d = payload[pos+3];
                    let (h, mi, s) = if dlen >= 7 && pos + 7 <= payload.len() {
                        (payload[pos+4], payload[pos+5], payload[pos+6])
                    } else { (0, 0, 0) };
                    pos += dlen;
                    format!("'{:04}-{:02}-{:02} {:02}:{:02}:{:02}'", y, mo, d, h, mi, s)
                } else {
                    pos += dlen;
                    "NULL".to_string()
                }
            }
            _ => { // VAR_STRING / BLOB / default: length-encoded string
                if pos >= payload.len() { params.push("NULL".to_string()); continue; }
                let (slen, nbytes) = read_lenenc(&payload[pos..]);
                pos += nbytes;
                if pos + slen > payload.len() { params.push("NULL".to_string()); continue; }
                let s = String::from_utf8_lossy(&payload[pos..pos+slen]).to_string();
                pos += slen;
                format!("'{}'", s.replace('\'', "''"))
            }
        };
        params.push(val);
    }
    params
}

fn read_lenenc(buf: &[u8]) -> (usize, usize) {
    match buf.first() {
        Some(&b) if b < 251 => (b as usize, 1),
        Some(0xfc) if buf.len() >= 3 => {
            let n = u16::from_le_bytes([buf[1], buf[2]]) as usize;
            (n, 3)
        }
        Some(0xfd) if buf.len() >= 4 => {
            let n = (buf[1] as usize) | ((buf[2] as usize) << 8) | ((buf[3] as usize) << 16);
            (n, 4)
        }
        _ => (0, 1),
    }
}

// ── Result set ─────────────────────────────────────────────────

/// Parse the box-draw table format produced by the executor.
/// Returns (column_names, rows) or None if not a table output.
fn parse_table(out: &str) -> Option<(Vec<String>, Vec<Vec<String>>)> {
    if !out.starts_with('+') { return None; }
    let mut cols: Vec<String> = Vec::new();
    let mut rows: Vec<Vec<String>> = Vec::new();
    let mut header = false;
    for line in out.lines() {
        if line.starts_with('+') { continue; }
        if line.starts_with('|') {
            let cells: Vec<String> = line
                .split('|')
                .filter(|s| !s.is_empty())
                .map(|s| s.trim().to_string())
                .collect();
            if !header { cols = cells; header = true; }
            else { rows.push(cells); }
        }
    }
    if cols.is_empty() { return None; }
    Some((cols, rows))
}

fn send_resultset(s: &mut TcpStream, cols: Vec<String>, rows: Vec<Vec<String>>, start_seq: u8) -> std::io::Result<()> {
    let mut seq = start_seq;

    // Column count packet
    let mut cnt = Vec::new();
    lenenc(&mut cnt, cols.len() as u64);
    write_packet(s, seq, &cnt)?; seq += 1;

    // Column definition packets
    for col in &cols { write_packet(s, seq, &col_def_pkt(col))?; seq += 1; }

    // EOF after column defs
    write_packet(s, seq, &eof_pkt())?; seq += 1;

    // Row data packets
    for row in &rows {
        let mut pkt = Vec::new();
        for i in 0..cols.len() {
            let v = row.get(i).map(|s| s.as_str()).unwrap_or("");
            if v == "NULL" { pkt.push(0xfb); }      // NULL indicator
            else { lenstr(&mut pkt, v.as_bytes()); }
        }
        write_packet(s, seq, &pkt)?; seq += 1;
    }

    // EOF after rows
    write_packet(s, seq, &eof_pkt())
}

// ── MySQL-specific compatibility helpers ───────────────────────

/// Build a box-drawing table string (same format as the engine outputs).
fn box_table(cols: &[&str], rows: &[Vec<String>]) -> String {
    let mut widths: Vec<usize> = cols.iter().map(|c| c.len()).collect();
    for row in rows {
        for (i, cell) in row.iter().enumerate() {
            if i < widths.len() { widths[i] = widths[i].max(cell.len()); }
        }
    }
    let sep: String = widths.iter()
        .map(|&w| format!("+{}", "-".repeat(w + 2)))
        .collect::<String>() + "+";
    let make_row = |cells: &[&str]| -> String {
        let mut s = String::new();
        for (i, &w) in widths.iter().enumerate() {
            s += &format!("| {:<w$} ", cells.get(i).copied().unwrap_or(""), w = w);
        }
        s + "|"
    };
    let mut out = vec![sep.clone()];
    out.push(make_row(cols));
    out.push(sep.clone());
    for row in rows {
        let refs: Vec<&str> = row.iter().map(|s| s.as_str()).collect();
        out.push(make_row(&refs));
    }
    out.push(sep);
    out.push(format!("{} row(s) returned.", rows.len()));
    out.join("\n")
}

/// Parse all @@[session.|global.]varname [AS alias] from a SELECT statement
/// and return a result set with known MySQL variable values.
fn handle_select_atvars(q: &str) -> String {
    let known: &[(&str, &str)] = &[
        ("auto_increment_increment", "1"),
        ("autocommit", "ON"),
        ("character_set_client", "utf8mb4"),
        ("character_set_connection", "utf8mb4"),
        ("character_set_results", "utf8mb4"),
        ("character_set_server", "utf8mb4"),
        ("collation_connection", "utf8mb4_general_ci"),
        ("collation_server", "utf8mb4_general_ci"),
        ("init_connect", ""),
        ("interactive_timeout", "28800"),
        ("license", "GPL"),
        ("lower_case_table_names", "0"),
        ("max_allowed_packet", "67108864"),
        ("net_write_timeout", "60"),
        ("query_cache_size", "0"),
        ("query_cache_type", "OFF"),
        ("sql_mode", ""),
        ("system_time_zone", "UTC"),
        ("time_zone", "SYSTEM"),
        ("transaction_isolation", "READ-COMMITTED"),
        ("tx_isolation", "READ-COMMITTED"),
        ("version", "5.7.0-rustdb"),
        ("version_comment", "RustDB"),
        ("wait_timeout", "28800"),
    ];
    let mut cols: Vec<String> = Vec::new();
    let mut vals: Vec<String> = Vec::new();
    let bytes = q.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if i + 1 < bytes.len() && bytes[i] == b'@' && bytes[i + 1] == b'@' {
            i += 2;
            let chunk_up = q[i..].to_uppercase();
            if chunk_up.starts_with("SESSION.") { i += 8; }
            else if chunk_up.starts_with("GLOBAL.") { i += 7; }
            let start = i;
            while i < bytes.len() && (bytes[i].is_ascii_alphanumeric() || bytes[i] == b'_') { i += 1; }
            let var_name = q[start..i].to_lowercase();
            while i < bytes.len() && bytes[i] == b' ' { i += 1; }
            let alias = if q[i..].to_uppercase().starts_with("AS ") {
                i += 3;
                while i < bytes.len() && bytes[i] == b' ' { i += 1; }
                let as0 = i;
                while i < bytes.len() && (bytes[i].is_ascii_alphanumeric() || bytes[i] == b'_') { i += 1; }
                q[as0..i].to_string()
            } else {
                var_name.clone()
            };
            let value = known.iter().find(|(k, _)| *k == var_name.as_str())
                .map(|(_, v)| v.to_string()).unwrap_or_default();
            cols.push(alias);
            vals.push(value);
        } else { i += 1; }
    }
    if cols.is_empty() {
        return box_table(&["result"], &[vec!["".to_string()]]);
    }
    let col_refs: Vec<&str> = cols.iter().map(|s| s.as_str()).collect();
    box_table(&col_refs, &[vals])
}

/// Simple SHOW VARIABLES response with meaningful values.
fn show_variables_result(up: &str) -> String {
    let vars: &[(&str, &str)] = &[
        ("autocommit", "ON"),
        ("character_set_client", "utf8mb4"),
        ("character_set_connection", "utf8mb4"),
        ("character_set_results", "utf8mb4"),
        ("character_set_server", "utf8mb4"),
        ("collation_connection", "utf8mb4_general_ci"),
        ("collation_server", "utf8mb4_general_ci"),
        ("interactive_timeout", "28800"),
        ("lower_case_table_names", "0"),
        ("max_allowed_packet", "67108864"),
        ("net_write_timeout", "60"),
        ("query_cache_size", "0"),
        ("query_cache_type", "OFF"),
        ("sql_mode", ""),
        ("system_time_zone", "UTC"),
        ("time_zone", "SYSTEM"),
        ("transaction_isolation", "READ-COMMITTED"),
        ("tx_isolation", "READ-COMMITTED"),
        ("version", "5.7.0-rustdb"),
        ("version_comment", "RustDB"),
        ("wait_timeout", "28800"),
    ];
    // Filter by LIKE pattern if present
    let like_pat = if let Some(idx) = up.find(" LIKE ") {
        let rest = &up[idx + 6..].trim_matches('\'');
        let end = rest.find('\'').unwrap_or(rest.len());
        Some(rest[..end].to_lowercase())
    } else { None };
    let rows: Vec<Vec<String>> = vars.iter()
        .filter(|(k, _)| like_pat.as_ref().map_or(true, |pat| like_match(k, pat)))
        .map(|(k, v)| vec![k.to_string(), v.to_string()])
        .collect();
    box_table(&["Variable_name", "Value"], &rows)
}

/// SQL LIKE pattern matching (% = any sequence, _ = one char).
fn like_match(s: &str, pat: &str) -> bool {
    fn inner(s: &[u8], p: &[u8]) -> bool {
        match (s.is_empty(), p.is_empty()) {
            (true, true) => return true,
            (_, true) => return false,
            _ => {}
        }
        if p[0] == b'%' { return inner(s, &p[1..]) || (!s.is_empty() && inner(&s[1..], p)); }
        if s.is_empty() { return false; }
        if p[0] == b'_' || p[0].to_ascii_lowercase() == s[0].to_ascii_lowercase() { return inner(&s[1..], &p[1..]); }
        false
    }
    inner(s.as_bytes(), pat.as_bytes())
}

/// Extract first identifier after the first " FROM " in q.
fn extract_first_from(q: &str) -> Option<String> {
    let up = q.to_uppercase();
    let idx = up.find(" FROM ")?;
    let rest = q[idx + 6..].trim().trim_start_matches('`');
    let end = rest.find(|c: char| !c.is_alphanumeric() && c != '_').unwrap_or(rest.len());
    let s = &rest[..end];
    if s.is_empty() || s.eq_ignore_ascii_case("WHERE") { None } else { Some(s.to_string()) }
}

/// Extract second identifier after the second " FROM " in q.
fn extract_second_from(q: &str) -> Option<String> {
    let up = q.to_uppercase();
    let first = up.find(" FROM ")?;
    let rest_up = &up[first + 6..];
    let second = rest_up.find(" FROM ")?;
    let pos = first + 6 + second + 6;
    let rest = q[pos..].trim().trim_start_matches('`');
    let end = rest.find(|c: char| !c.is_alphanumeric() && c != '_').unwrap_or(rest.len());
    let s = &rest[..end];
    if s.is_empty() { None } else { Some(s.to_string()) }
}

/// Execute a query directly through the parser+executor (no compat shims).
fn exec_inner(exec: &mut Executor, q: &str) -> Result<String, String> {
    let mut p = Parser::new(q.trim());
    match p.parse() {
        Ok(stmt) => exec.execute(stmt),
        Err(e) => Err(format!("Parse Error: {}", e)),
    }
}

// ── MySQL-specific compatibility shims ─────────────────────────

/// Handle MySQL system queries our engine doesn't parse.
/// Returns Some(output) to short-circuit the engine, or None to proceed normally.
fn mysql_compat(q: &str, exec: &mut Executor) -> Option<Result<String, String>> {
    let up = q.trim().to_uppercase();
    let up = up.trim();

    // ─── SET (charset, autocommit, session vars) ───────────────
    if up.starts_with("SET ") { return Some(Ok(String::new())); }

    // ─── SELECT VERSION() ──────────────────────────────────────
    if up == "SELECT VERSION()" || up.starts_with("SELECT VERSION() ")
        || up == "SELECT @@VERSION" || up.starts_with("SELECT @@VERSION ")
    {
        return Some(Ok(box_table(&["version"], &[vec!["5.7.0-rustdb".to_string()]])));
    }

    // ─── SELECT USER() / CURRENT_USER() ───────────────────────
    if up == "SELECT USER()" || up == "SELECT CURRENT_USER()"
        || up == "SELECT USER() AS USER" || up == "SELECT CURRENT_USER() AS USER"
    {
        return Some(Ok(box_table(&["user()"], &[vec!["root@localhost".to_string()]])));
    }

    // ─── SELECT DATABASE() / SELECT SCHEMA() ──────────────────
    if up == "SELECT DATABASE()" || up == "SELECT SCHEMA()"
        || up == "SELECT DATABASE() AS DATABASE" || up == "SELECT SCHEMA() AS SCHEMA"
    {
        // Try engine first (it knows current DB)
        if let Ok(out) = exec_inner(exec, "SELECT DATABASE()") {
            if !out.is_empty() { return Some(Ok(out)); }
        }
        return Some(Ok(box_table(&["DATABASE()"], &[vec!["".to_string()]])));
    }

    // ─── SELECT @@var(s) ───────────────────────────────────────
    // Handle single and multi-variable SELECT @@ queries
    if up.starts_with("SELECT @@") || (up.starts_with("SELECT") && up.contains("@@") && !up.contains(" FROM ")) {
        return Some(Ok(handle_select_atvars(q)));
    }

    // ─── SHOW VARIABLES ───────────────────────────────────────
    if up.starts_with("SHOW VARIABLES") || up.starts_with("SHOW SESSION VARIABLES")
        || up.starts_with("SHOW GLOBAL VARIABLES")
    {
        return Some(Ok(show_variables_result(up)));
    }

    // ─── SHOW STATUS ──────────────────────────────────────────
    if up.starts_with("SHOW STATUS") || up.starts_with("SHOW SESSION STATUS")
        || up.starts_with("SHOW GLOBAL STATUS")
    {
        return Some(Ok(box_table(&["Variable_name", "Value"], &[])));
    }

    // ─── SHOW COLLATION ───────────────────────────────────────
    if up.starts_with("SHOW COLLATION") {
        let rows = vec![
            vec!["utf8_general_ci","utf8","33","Yes","Yes","1"],
            vec!["utf8mb4_general_ci","utf8mb4","45","Yes","Yes","1"],
            vec!["utf8mb4_unicode_ci","utf8mb4","224","","Yes","8"],
            vec!["latin1_swedish_ci","latin1","8","Yes","Yes","1"],
        ];
        let rows: Vec<Vec<String>> = rows.iter()
            .map(|r| r.iter().map(|s| s.to_string()).collect())
            .collect();
        return Some(Ok(box_table(&["Collation","Charset","Id","Default","Compiled","Sortlen"], &rows)));
    }

    // ─── SHOW CHARACTER SET / CHARSET ─────────────────────────
    if up.starts_with("SHOW CHARSET") || up.starts_with("SHOW CHARACTER SET") {
        let rows = vec![
            vec!["utf8".to_string(), "UTF-8 Unicode".to_string(), "utf8_general_ci".to_string(), "3".to_string()],
            vec!["utf8mb4".to_string(), "UTF-8 Unicode".to_string(), "utf8mb4_general_ci".to_string(), "4".to_string()],
            vec!["latin1".to_string(), "cp1252 West European".to_string(), "latin1_swedish_ci".to_string(), "1".to_string()],
        ];
        return Some(Ok(box_table(&["Charset","Description","Default collation","Maxlen"], &rows)));
    }

    // ─── SHOW ENGINES ─────────────────────────────────────────
    if up.starts_with("SHOW ENGINES") {
        let rows = vec![vec![
            "RustDB".to_string(), "DEFAULT".to_string(),
            "RustDB B+Tree Storage Engine".to_string(),
            "YES".to_string(), "YES".to_string(), "YES".to_string(),
        ]];
        return Some(Ok(box_table(&["Engine","Support","Comment","Transactions","XA","Savepoints"], &rows)));
    }

    // ─── SHOW PLUGINS / WARNINGS / EVENTS ─────────────────────
    if up.starts_with("SHOW PLUGINS") {
        return Some(Ok(box_table(&["Name","Status","Type","Library","License"], &[])));
    }
    if up.starts_with("SHOW WARNINGS") {
        return Some(Ok(box_table(&["Level","Code","Message"], &[])));
    }
    if up.starts_with("SHOW EVENTS") {
        return Some(Ok(box_table(&["Db","Name","Definer","Time zone","Type","Execute at","Interval value","Interval field","Starts","Ends","Status","Originator"], &[])));
    }

    // ─── SHOW FUNCTION STATUS / PROCEDURE STATUS ───────────────
    if up.starts_with("SHOW FUNCTION STATUS") || up.starts_with("SHOW PROCEDURE STATUS") {
        return Some(Ok(box_table(&["Db","Name","Type","Definer","Modified","Created","Security_type","Comment","character_set_client","collation_connection","Database Collation"], &[])));
    }

    // ─── SHOW TABLE STATUS ────────────────────────────────────
    if up.starts_with("SHOW TABLE STATUS") {
        return Some(Ok(box_table(&["Name","Engine","Version","Row_format","Rows","Avg_row_length","Data_length","Max_data_length","Index_length","Data_free","Auto_increment","Create_time","Update_time","Check_time","Collation","Checksum","Create_options","Comment"], &[])));
    }

    // ─── SHOW TRIGGERS ────────────────────────────────────────
    if up.starts_with("SHOW TRIGGERS") {
        return Some(Ok(box_table(&["Trigger","Event","Table","Statement","Timing","Created","sql_mode","Definer","character_set_client","collation_connection","Database Collation"], &[])));
    }

    // ─── SHOW INDEX / INDEXES / KEYS FROM table ───────────────
    if up.starts_with("SHOW INDEX FROM") || up.starts_with("SHOW INDEXES FROM")
        || up.starts_with("SHOW KEYS FROM")
    {
        return Some(Ok(box_table(&["Table","Non_unique","Key_name","Seq_in_index","Column_name","Collation","Cardinality","Sub_part","Packed","Null","Index_type","Comment","Index_comment"], &[])));
    }

    // ─── SHOW FULL TABLES [FROM db] [WHERE table_type=...] ────
    if up.starts_with("SHOW FULL TABLES") {
        // Extract optional db name from "FROM db" portion
        let db_opt = extract_first_from(q);
        // Determine Table_type filter from WHERE clause
        let type_filter: Option<&str> = if up.contains("'VIEW'") { Some("VIEW") }
            else if up.contains("'BASE TABLE'") { Some("BASE TABLE") }
            else { None };

        // Execute SHOW TABLES [FROM db] through engine
        let tbl_q = match &db_opt {
            Some(db) => format!("SHOW TABLES FROM {}", db),
            None => "SHOW TABLES".to_string(),
        };
        let (tbl_col, tbl_names) = exec_inner(exec, &tbl_q)
            .ok()
            .and_then(|out| parse_table(&out))
            .map(|(cols, rows)| {
                let col = cols.into_iter().next().unwrap_or_else(|| "Tables".to_string());
                let names: Vec<String> = rows.into_iter()
                    .filter_map(|r| r.into_iter().next())
                    .collect();
                (col, names)
            })
            .unwrap_or_else(|| ("Tables".to_string(), vec![]));

        let full_col = format!("Tables_in_{}", db_opt.as_deref().unwrap_or(""));
        let _ = tbl_col; // suppress warning

        // All engine tables are BASE TABLE (views tracked separately via information_schema)
        let rows: Vec<Vec<String>> = tbl_names.into_iter()
            .filter(|_| type_filter.map_or(true, |f| f == "BASE TABLE"))
            .map(|name| vec![name, "BASE TABLE".to_string()])
            .collect();

        return Some(Ok(box_table(&[&full_col, "Table_type"], &rows)));
    }

    // ─── SHOW FULL COLUMNS FROM table [FROM db] ───────────────
    if up.starts_with("SHOW FULL COLUMNS") || up.starts_with("SHOW COLUMNS") {
        let tbl_opt = extract_first_from(q);
        let db_opt = extract_second_from(q);
        let tbl = match tbl_opt {
            Some(t) => t,
            None => return Some(Ok(box_table(&["Field","Type","Collation","Null","Key","Default","Extra","Privileges","Comment"], &[]))),
        };
        let desc_q = match &db_opt {
            Some(db) => format!("DESCRIBE {}.{}", db, tbl),
            None => format!("DESCRIBE {}", tbl),
        };
        let (cols, rows) = exec_inner(exec, &desc_q)
            .ok()
            .and_then(|out| parse_table(&out))
            .unwrap_or_else(|| (vec![], vec![]));

        // DESCRIBE → Field, Type, Null, Key, Default, Extra
        let get = |row: &Vec<String>, name: &str| -> String {
            cols.iter().position(|c| c.eq_ignore_ascii_case(name))
                .and_then(|i| row.get(i)).cloned().unwrap_or_default()
        };
        let new_rows: Vec<Vec<String>> = rows.iter().map(|row| vec![
            get(row, "Field"),
            get(row, "Type"),
            "utf8mb4_general_ci".to_string(),
            get(row, "Null"),
            get(row, "Key"),
            get(row, "Default"),
            get(row, "Extra"),
            "select,insert,update,references".to_string(),
            "".to_string(),
        ]).collect();
        return Some(Ok(box_table(&["Field","Type","Collation","Null","Key","Default","Extra","Privileges","Comment"], &new_rows)));
    }

    None
}

// ── Query execution ────────────────────────────────────────────

fn exec_query(exec: &mut Executor, raw: &str) -> Result<String, String> {
    let q = raw.trim().trim_end_matches(';').trim();
    if q.is_empty() { return Ok(String::new()); }

    if let Some(r) = mysql_compat(q, exec) { return r; }

    let mut p = Parser::new(q);
    match p.parse() {
        Ok(stmt) => exec.execute(stmt),
        Err(e) => Err(format!("Parse Error: {}", e)),
    }
}

fn affected_rows(msg: &str) -> u64 {
    msg.split_whitespace().next().and_then(|s| s.parse().ok()).unwrap_or(0)
}

// ── Client handler ─────────────────────────────────────────────

pub fn handle_mysql_client(mut stream: TcpStream, shared: Arc<RwLock<SharedDatabase>>, conn_id: u32) {
    // 1. Server sends Handshake
    if write_packet(&mut stream, 0, &handshake_pkt(conn_id)).is_err() { return; }

    // 2. Client sends HandshakeResponse
    let (_, resp) = match read_packet(&mut stream) { Some(r) => r, None => return };

    // Parse: [cap:4][max_pkt:4][charset:1][reserved:23] → 32 bytes; then username\0
    if resp.len() < 32 { return; }
    let client_caps = u32::from_le_bytes([resp[0], resp[1], resp[2], resp[3]]);
    let mut pos = 32;

    // username (null-terminated)
    let uend = resp[pos..].iter().position(|&b| b == 0).unwrap_or(resp.len() - pos);
    let _username = String::from_utf8_lossy(&resp[pos..pos + uend]).to_string();
    pos += uend + 1;

    // auth-response (skipped, not verified)
    if client_caps & 0x8000 != 0 {
        // CLIENT_SECURE_CONNECTION: 1-byte length prefix
        let auth_len = resp.get(pos).copied().unwrap_or(0) as usize;
        pos += 1 + auth_len;
    } else {
        // null-terminated
        while pos < resp.len() && resp[pos] != 0 { pos += 1; }
        pos += 1;
    }

    // initial database (if CLIENT_CONNECT_WITH_DB)
    let init_db = if client_caps & 0x0008 != 0 && pos < resp.len() {
        let end = resp[pos..].iter().position(|&b| b == 0).unwrap_or(resp.len() - pos);
        let db = String::from_utf8_lossy(&resp[pos..pos + end]).to_string();
        db
    } else {
        String::new()
    };

    // 3. Server sends OK (auth not verified)
    if write_packet(&mut stream, 2, &ok_pkt(0)).is_err() { return; }

    // 4. Query loop
    let mut exec = Executor::new_session(Arc::clone(&shared));
    let mut stmts: StmtMap = StmtMap::new();
    let mut next_stmt_id: u32 = 1;

    if !init_db.is_empty() {
        let _ = exec_query(&mut exec, &format!("USE {}", init_db));
    }

    loop {
        let (_, payload) = match read_packet(&mut stream) { Some(r) => r, None => break };
        if payload.is_empty() { break; }

        let cmd = payload[0];
        match cmd {
            COM_QUIT => break,

            COM_PING => {
                let _ = write_packet(&mut stream, 1, &ok_pkt(0));
            }

            COM_INIT_DB => {
                let db = String::from_utf8_lossy(&payload[1..]).trim_matches('\0').to_string();
                match exec_query(&mut exec, &format!("USE {}", db)) {
                    Ok(_) => { let _ = write_packet(&mut stream, 1, &ok_pkt(0)); }
                    Err(e) => { let _ = write_packet(&mut stream, 1, &err_pkt(&e)); }
                }
            }

            COM_QUERY => {
                let query = String::from_utf8_lossy(&payload[1..]).to_string();
                let query = query.trim().trim_end_matches(';').trim().to_string();
                match exec_query(&mut exec, &query) {
                    Err(e) => {
                        let _ = write_packet(&mut stream, 1, &err_pkt(&e));
                    }
                    Ok(out) => {
                        if out.is_empty() {
                            let _ = write_packet(&mut stream, 1, &ok_pkt(0));
                        } else if let Some((cols, rows)) = parse_table(&out) {
                            let _ = send_resultset(&mut stream, cols, rows, 1);
                        } else {
                            let n = affected_rows(&out);
                            let _ = write_packet(&mut stream, 1, &ok_pkt(n));
                        }
                    }
                }
            }

            COM_STMT_PREPARE => {
                let query = String::from_utf8_lossy(&payload[1..]).trim_matches('\0').to_string();
                let num_params = count_placeholders(&query);
                let stmt_id = next_stmt_id;
                next_stmt_id += 1;
                stmts.insert(stmt_id, PreparedStmt { query, num_params });

                let mut seq: u8 = 1;
                let _ = write_packet(&mut stream, seq, &stmt_prepare_ok(stmt_id, num_params as u16, 0));
                seq += 1;
                // Send param column defs + EOF if num_params > 0
                if num_params > 0 {
                    for _ in 0..num_params {
                        let _ = write_packet(&mut stream, seq, &col_def_pkt("?"));
                        seq += 1;
                    }
                    let _ = write_packet(&mut stream, seq, &eof_pkt());
                }
            }

            COM_STMT_EXECUTE => {
                if payload.len() < 5 {
                    let _ = write_packet(&mut stream, 1, &err_pkt("Invalid COM_STMT_EXECUTE"));
                    continue;
                }
                let stmt_id = u32::from_le_bytes([payload[1], payload[2], payload[3], payload[4]]);
                let (query, num_params) = match stmts.get(&stmt_id) {
                    Some(s) => (s.query.clone(), s.num_params),
                    None => {
                        let _ = write_packet(&mut stream, 1, &err_pkt("Unknown prepared statement"));
                        continue;
                    }
                };
                let params = parse_execute_params(&payload[1..], num_params);
                let final_query = bind_params(&query, &params);
                match exec_query(&mut exec, &final_query) {
                    Err(e) => { let _ = write_packet(&mut stream, 1, &err_pkt(&e)); }
                    Ok(out) => {
                        if out.is_empty() {
                            let _ = write_packet(&mut stream, 1, &ok_pkt(0));
                        } else if let Some((cols, rows)) = parse_table(&out) {
                            let _ = send_resultset(&mut stream, cols, rows, 1);
                        } else {
                            let n = affected_rows(&out);
                            let _ = write_packet(&mut stream, 1, &ok_pkt(n));
                        }
                    }
                }
            }

            COM_STMT_CLOSE => {
                if payload.len() >= 5 {
                    let stmt_id = u32::from_le_bytes([payload[1], payload[2], payload[3], payload[4]]);
                    stmts.remove(&stmt_id);
                }
                // No response for COM_STMT_CLOSE
            }

            COM_STMT_RESET => {
                let _ = write_packet(&mut stream, 1, &ok_pkt(0));
            }

            _ => {
                let _ = write_packet(&mut stream, 1, &err_pkt("Unsupported command"));
            }
        }
    }
}

// ── Listener ───────────────────────────────────────────────────

pub fn start_mysql_listener(port: u16, shared: Arc<RwLock<SharedDatabase>>) {
    let addr = format!("0.0.0.0:{}", port);
    let listener = match TcpListener::bind(&addr) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("[mysql] Failed to bind {}: {}", addr, e);
            return;
        }
    };

    println!("|   MySQL protocol on 0.0.0.0:{:<16}|", port);

    let counter = Arc::new(AtomicUsize::new(1));
    thread::spawn(move || {
        for stream in listener.incoming() {
            match stream {
                Ok(s) => {
                    let sh = Arc::clone(&shared);
                    let id = counter.fetch_add(1, Ordering::SeqCst) as u32;
                    thread::spawn(move || handle_mysql_client(s, sh, id));
                }
                Err(_) => break,
            }
        }
    });
}
