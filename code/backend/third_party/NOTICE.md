# Vendored third-party libraries

Per the migration plan (package-manager-free, vendoring-heavy approach following
MySQL/PostgreSQL/SQLite precedent), these libraries are vendored directly rather than
pulled via vcpkg/Conan.

| Library | Path | Version | License | Replaces (Rust crate) |
|---|---|---|---|---|
| nlohmann/json | `nlohmann/json.hpp` | v3.11.3 | MIT | serde / serde_json |
| lz4 | `lz4/lz4.c`, `lz4/lz4.h` | v1.9.4 (lib/ only, block API) | BSD 2-Clause | lz4_flex |
| Catch2 | `catch2/catch.hpp` | v2.13.10 (last single-header release) | BSL-1.0 | `#[test]` |
| sha1 (vog/sha1) | `sha1/sha1.hpp` (header-only; `sha1.cpp` kept empty for back-compat) | master (2024) | Public Domain | sha1 |
| PicoSHA2 | `picosha2/picosha2.h` | master (2024) | MIT | sha2 (SHA-256) |

Sources fetched from each project's official GitHub repository/releases. Do not hand-edit
these files; if a fix is needed, update the version pin above and re-fetch.
