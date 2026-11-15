"""Validate the static pinyin dictionary used by app_ui_ime.

The dictionary is generated from the MIT-licensed 100ask data.  The app-owned
T9 engine assumes each entry has a lowercase pinyin key and that every
candidate string is valid UTF-8 Chinese; a malformed entry would make a pinyin
syllable silently lose candidates.
"""

from pathlib import Path
import re
import unittest


PROJECT_ROOT = Path(__file__).resolve().parents[2]
DICT_PATH = (PROJECT_ROOT / "layers" / "apps" / "common" / "src" /
             "app_ui_ime_dict.c")
ENTRY_RE = re.compile(r'\{\s*"([a-z]+)"\s*,\s*"([^"]*)"\s*\}')
MIN_ENTRIES = 390
MIN_CHARS = 4000
T9_DIGITS = {
    **dict.fromkeys("abc", "2"),
    **dict.fromkeys("def", "3"),
    **dict.fromkeys("ghi", "4"),
    **dict.fromkeys("jkl", "5"),
    **dict.fromkeys("mno", "6"),
    **dict.fromkeys("pqrs", "7"),
    **dict.fromkeys("tuv", "8"),
    **dict.fromkeys("wxyz", "9"),
}


def t9_sequence(pinyin: str) -> str:
    """Return the T9 digit sequence for one pinyin spelling."""
    return "".join(T9_DIGITS[letter] for letter in pinyin)


class ImeDictionaryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.text = DICT_PATH.read_text(encoding="utf-8")
        self.entries = ENTRY_RE.findall(self.text)

    def test_dictionary_is_complete(self) -> None:
        self.assertGreaterEqual(len(self.entries), MIN_ENTRIES)
        characters = sum(len(chars) for _, chars in self.entries)
        self.assertGreaterEqual(characters, MIN_CHARS)

    def test_entries_are_sorted_and_unique(self) -> None:
        pinyin = [py for py, _ in self.entries]
        self.assertEqual(pinyin, sorted(pinyin))
        self.assertEqual(len(pinyin), len(set(pinyin)))
        self.assertIn("{ NULL, NULL },", self.text)

    def test_candidates_are_common_cjk(self) -> None:
        for pinyin, chars in self.entries:
            self.assertTrue(chars, pinyin)
            for char in chars:
                self.assertGreaterEqual(ord(char), 0x4E00, pinyin)
                self.assertLessEqual(ord(char), 0x9FFF, pinyin)

    def test_every_pinyin_has_a_t9_sequence(self) -> None:
        for pinyin, _ in self.entries:
            digits = t9_sequence(pinyin)
            self.assertTrue(digits, pinyin)
            self.assertTrue(all("2" <= digit <= "9" for digit in digits), pinyin)

    def test_t9_ambiguity_contains_ca(self) -> None:
        groups = {}
        for pinyin, chars in self.entries:
            groups.setdefault(t9_sequence(pinyin), []).append((pinyin, chars))
        self.assertIn("ba", [pinyin for pinyin, _ in groups["22"]])
        self.assertIn("ca", [pinyin for pinyin, _ in groups["22"]])
        ca_chars = next(chars for pinyin, chars in groups["22"] if pinyin == "ca")
        self.assertIn("擦", ca_chars)
        self.assertIn("can", [pinyin for pinyin, _ in groups["226"]])
        self.assertIn("cao", [pinyin for pinyin, _ in groups["226"]])


if __name__ == "__main__":
    unittest.main()
