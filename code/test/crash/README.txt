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

  python fuzz_crash_fk.py [rounds] [base_seed]    the same for a parent table p and a child table c with a FOREIGN KEY whose ON DELETE / ON UPDATE actions
                                                  (RESTRICT, CASCADE, SET NULL) the seed picks: child/parent INSERT, REPLACE, IGNORE, ON DUPLICATE KEY UPDATE,
                                                  UPDATE, DELETE, transactions and savepoints, checked against a model of the actions; after recovery a child
                                                  of a missing parent and (RESTRICT) the delete of a parent that has a child must still be refused.
                                                  FUZZ_TRACE=1 prints every statement.

  python fuzz_crash_selfref.py [rounds] [base_seed]  the same for one table emp whose foreign key points at itself (emp.mgr -> emp.id) with the ON DELETE action the seed picks: INSERT of rows
                                                  under another row, UPDATE of v / mgr / the key, DELETE of one row or several (`id IN (...)`, ascending or ORDER BY id DESC), transactions and
                                                  savepoints, checked against a model of InnoDB's row-by-row work (a refused statement changes nothing, a cascade follows the children, at most
                                                  14 levels); after recovery a row under a missing row and (RESTRICT) the delete of a manager with a report must still be refused.

  python fuzz_crash_chain.py [rounds] [base_seed]    the same for a chain of three tables a <- b <- c whose two foreign keys have the ON DELETE actions the seed picks (RESTRICT, CASCADE,
                                                  SET NULL): INSERT of b / c rows under another row or none, UPDATE of v and of the foreign key columns, DELETE of one row or several of a, b and c, transactions
                                                  and savepoints, checked against a model of the cascade (the grandchildren go with the children, a RESTRICT anywhere below refuses the statement whole);
                                                  after recovery a row under a missing row and (RESTRICT) the delete of a row that a child refers to must still be refused.

  python fuzz_crash_multifk.py [rounds] [base_seed]  the same for a column that is in two foreign keys: parents pa and pb, a child c whose x refers to both (the ON DELETE actions the seed picks, RESTRICT /
                                                  CASCADE / SET NULL for each): INSERT / UPDATE of c under a value that is in both parents, in one or in none (refused unless it is in both), DELETE of one row or
                                                  several of pa, pb and c, REPLACE of one or several parent rows (the replaced row goes through its keys first), transactions and savepoints, checked against a
                                                  model of the two keys; after recovery a value that is in one parent only must still be refused by the other key.

After recovery fuzz_crash.py also checks the INDEXES against the recovered rows (a lookup through the secondary index
g and through the primary key must return what the oracle says) -- a table scan alone cannot see a stale index.

Env: RUSQL_REDO_CHECKPOINT_BYTES=3000  forces frequent redo checkpoints between kills (default 4MB)
     RUSQL_REDO_CHECKPOINT_ROW_BYTES=<n>  a checkpoint is due once the redo log passes max(CHECKPOINT_BYTES, n x rows of the
                                       tables changed since the last one); server default 128. Both fuzzers start the server
                                       with 0 (fixed threshold) -- set 128 to fuzz recovery from a long redo log instead
     RUSQL_DML_INDEX_MIN_ROWS=<n>      UPDATE/DELETE use indexes only on tables of n+ rows (server default 64);
                                       fuzz_crash.py starts the server with 0 so its tiny tables take the index
                                       paths -- set a huge number (e.g. 999999) for a scan-only control run
     RUSQL_SERVER_EXE=<path>           use another build (default: build/backend/server/Release)
Exit code 1 and a MISMATCH line (seed, table, expected vs recovered) on any inconsistency.
A seed is deterministic: rerun with the same base_seed to reproduce.
