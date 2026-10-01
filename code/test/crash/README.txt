Crash-consistency fuzzers (redo-log durability)
================================================
Both start the real engine_server, drive random DML over the native protocol, kill -9 it at a
random moment, restart it and compare the recovered tables with an oracle of what the server
acknowledged.

  python fuzz_crash.py [rounds] [base_seed]       sequential: autocommit + explicit txns, COMMIT/ROLLBACK,
                                                  REPLACE / ON DUPLICATE KEY / TRUNCATE (legacy paths), and
                                                  whole-group UPDATE/DELETE through a secondary index (g = id % 5)
  python fuzz_concurrent.py [rounds] [base_seed]  4 concurrent clients on one table; in-flight statement may
                                                  land or not, explicit transactions must be all-or-nothing

Env: RUSQL_REDO_CHECKPOINT_BYTES=3000  forces frequent redo checkpoints between kills (default 4MB)
     RUSQL_DML_INDEX_MIN_ROWS=<n>      UPDATE/DELETE use indexes only on tables of n+ rows (server default 64);
                                       fuzz_crash.py starts the server with 0 so its tiny tables take the index
                                       paths -- set a huge number (e.g. 999999) for a scan-only control run
     RUSQL_SERVER_EXE=<path>           use another build (default: build/backend/server/Release)
Exit code 1 and a MISMATCH line (seed, table, expected vs recovered) on any inconsistency.
A seed is deterministic: rerun with the same base_seed to reproduce.
