The default collation of MySQL 8.4, utf8mb4_0900_ai_ci (src/collation.cpp, src/collation_data.inc)
===================================================================================================
The weights are not computed from the Unicode tables: they are what a real MySQL says. All four tools need pymysql and a MySQL 8.4 on 127.0.0.1:3391
(user root, no password).

  python dump_weights.py weights.json        HEX(WEIGHT_STRING(char)) of every code point U+0001..U+10FFFF (surrogates left out), about 10 seconds
  python gen_collation_data.py weights.json ../../backend/core/src/collation_data.inc
                                             the table the engine compiles in: runs of code points with their weights; the Hangul syllables, the
                                             ideographs and the unassigned code points are left to the rules in collation.cpp (a code point whose weights
                                             differ from the rule is a run of its own)
  python gen_collation_cases.py ../../backend/tests/collation_cases.inc [pairs] [seed]
                                             pairs of texts with the answer of MySQL's STRCMP: random texts of many scripts, the edges of the runs of the
                                             table, Hangul syllables and their jamo, the edges of the ideograph blocks
  python gen_ascii_model.py weights.json ../diff/collation_model.py
                                             the ASCII part of the weights as a small Python module for the verify_*.py tools

Checks
  engine_tests "[collation]"                                  the pairs, the keys, LIKE, the SQL level, locks, partitions, indexes
  RUSQL_COLLATION_WEIGHTS=<weights.json> engine_tests "[weights]"
                                             every code point of the dump against the sort key the engine makes of it (1,112,065 of them)

Not done: a column or an expression with its own collation (COLLATE, BINARY) -- the parser refuses them.
