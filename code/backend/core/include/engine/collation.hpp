#pragma once

// MySQL's default collation, utf8mb4_0900_ai_ci: how two texts compare when nothing says otherwise.
//
// "ai_ci" is accent-insensitive and case-insensitive: 'a' = 'A' = 'á' = 'Ａ' (a fullwidth a), 'ß' = 'ss', 'æ' = 'ae', hiragana = katakana; a space is a character
// like any other and counts ("0900" collations have NO PAD: 'a' <> 'a '); the control characters and the combining accents are ignored. Texts are ordered by the
// weights of their characters in the Unicode Collation Algorithm (UCA 9.0): spaces, punctuation and symbols, then digits, then the letters of each script in
// their alphabetical order -- so '_' sorts before '0' and '0' before 'a', which a byte order does not. The weights are the ones WEIGHT_STRING() of a real MySQL
// gives (src/collation_data.inc, made by code/test/collation/); the Hangul syllables, the ideographs and the code points nobody has assigned get theirs by the
// rules of the algorithm.
//
// The text "NULL" is the engine's NULL, not a word: it is the same value as itself only, it sorts before every text, and 'null' / 'Null' are texts like any other.
//
// Everything that decides whether two values are the same value -- a comparison, ORDER BY, MIN / MAX, GROUP BY, DISTINCT, IN, a join key, UNIQUE / PRIMARY KEY, an
// index -- goes through the four functions here.

#include <string>
#include <string_view>

namespace engine::collation {

/// <0, 0 or >0: how `a` and `b` compare. 0 means they are the same value for every purpose.
int compare(std::string_view a, std::string_view b);

bool equal(std::string_view a, std::string_view b);

/// A text such that two texts are `equal` exactly when their keys are the same bytes (the key of a plain ASCII text is the text in lower case): for hash buckets, sets and
/// the keys of a grouping. It does NOT keep the order.
std::string key(std::string_view s);

/// The weights of the text as big-endian 16-bit numbers: the byte order of two sort keys is `compare`. (NULL is not given a place: ORDER BY puts it first itself.)
std::string sort_key(std::string_view s);

/// `value LIKE pattern`: `%` is any run of characters and `_` one character; two characters match when they have the same weights ('é' matches 'E').
bool like(std::string_view value, std::string_view pattern);

} // namespace engine::collation
