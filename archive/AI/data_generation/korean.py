"""Minimal Hangul particle selection so generated Korean questions read naturally
(이/가, 은/는, 을/를, 과/와 depend on whether the preceding syllable has a batchim)."""

_HANGUL_BASE = 0xAC00
_HANGUL_LAST = 0xD7A3


def _has_batchim(word: str) -> bool:
    if not word:
        return True
    ch = word[-1]
    code = ord(ch)
    if _HANGUL_BASE <= code <= _HANGUL_LAST:
        return (code - _HANGUL_BASE) % 28 != 0
    # Non-Hangul (romanized terms/acronyms/digits): default to "has batchim" form,
    # a reasonable approximation for this project's synthetic-data scope.
    return True


def eun_neun(word: str) -> str:
    return f"{word}은" if _has_batchim(word) else f"{word}는"


def i_ga(word: str) -> str:
    return f"{word}이" if _has_batchim(word) else f"{word}가"


def eul_reul(word: str) -> str:
    return f"{word}을" if _has_batchim(word) else f"{word}를"


def gwa_wa(word: str) -> str:
    return f"{word}과" if _has_batchim(word) else f"{word}와"


def ro_euro(word: str) -> str:
    """으로/로 (instrumental/directional particle) - a batchim of ㄹ takes 로 like no batchim."""
    if _has_batchim(word) and not word.endswith("ㄹ") and not (word and (ord(word[-1]) - _HANGUL_BASE) % 28 == 8):
        return f"{word}으로"
    return f"{word}로"
