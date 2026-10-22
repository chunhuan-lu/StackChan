"""Test and build commands: recognise them in shell command lines and summarise their output (pure functions).

Used for the robot's `pass` / `fail` events (REQ-003 protocol v1.1) from Claude Code's Bash tool and from sc-run.
"""

from __future__ import annotations

import re
import shlex
from dataclasses import dataclass
from typing import Iterable

# (kind, regex), matched at the start of each command segment (split at ; && || |) after leading environment
# assignments and wrappers (time, uv run, poetry run, npx, pnpm, yarn, ...) are removed; tried with the first word
# reduced to its basename without .exe/.cmd/.bat and as written. Extend with the config key "test_patterns" (extra
# regexes, kind "test").
DEFAULT_PATTERNS: tuple[tuple[str, str], ...] = (
    ("test", r"pytest\b"),
    ("test", r"py(?:thon)?[\d.]*\s+(?:-\S+\s+)*-m\s+(?:pytest|unittest)\b"),
    ("test", r"(?:tox|nox)\b"),
    ("test", r"ctest\b"),
    ("test", r"make\b.*\s(?:test|check)\b"),
    ("build", r"cmake\s+--build\b"),
    ("build", r"ninja\b"),
    ("test", r"(?:npm|pnpm|yarn)\s+(?:run\s+)?test\b"),
    ("test", r"(?:jest|vitest)\b"),
    ("test", r"cargo\s+test\b"),
    ("build", r"cargo\s+build\b"),
    ("test", r"go\s+test\b"),
    ("build", r"go\s+build\b"),
    ("build", r"idf\.py\b.*\s(?:build|flash|app-flash)\b"),
    ("test", r"mvn\b.*\stest\b"),
    ("test", r"gradlew?\b.*\stest\b"),
    ("test", r"dotnet\s+test\b"),
)

# Words removed in front of a command, longest first. "pnpm"/"yarn" alone are only removed when the rest does not
# match as it is ("pnpm test" is matched before "pnpm vitest" is reduced to "vitest").
_WRAPPERS: tuple[tuple[str, ...], ...] = (
    ("uv", "run"), ("poetry", "run"), ("pnpm", "exec"), ("pnpm", "dlx"), ("yarn", "exec"), ("yarn", "dlx"),
    ("npx",), ("pnpm",), ("yarn",),
)
_PREFIX_WORDS = {"time", "env", "command", "exec", "nohup"}
_ENV_ASSIGNMENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*=")
_ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_TAIL_CHARS = 64 * 1024  # summaries are at the end of the output


class CommandDetector:
    """Classifies a shell command line as "test", "build" or None."""

    def __init__(self, extra_patterns: Iterable[str] = ()) -> None:
        patterns = list(DEFAULT_PATTERNS) + [("test", p) for p in extra_patterns]
        self._patterns = [(kind, re.compile(regex)) for kind, regex in patterns]

    def kind(self, command: object) -> str | None:
        """ "test" when any segment runs tests, else "build" when one builds, else None."""
        if not isinstance(command, str) or not command.strip():
            return None
        kinds = {self._segment_kind(tokens) for tokens in _segments(command)}
        return "test" if "test" in kinds else "build" if "build" in kinds else None

    def _segment_kind(self, tokens: list[str]) -> str | None:
        words = _strip_prefix(tokens)
        while words:
            texts = (" ".join([_basename(words[0])] + words[1:]), " ".join(words))  # normalised and as written
            for kind, pattern in self._patterns:
                if any(pattern.match(text) for text in texts):
                    return kind
            wrapper = next((w for w in _WRAPPERS if tuple(words[: len(w)]) == w and len(words) > len(w)), None)
            if wrapper is None:
                return None
            words = _strip_prefix(words[len(wrapper) :])
        return None


def _segments(command: str) -> list[list[str]]:
    command = command.replace("\n", " ; ")  # multi-line commands: one segment per line
    try:
        lexer = shlex.shlex(command, posix=True, punctuation_chars=True)
        lexer.whitespace_split = True
        tokens = list(lexer)
    except ValueError:  # unbalanced quotes
        tokens = command.split()
    segments: list[list[str]] = [[]]
    for token in tokens:
        if token and set(token) <= set(";&|\n"):
            segments.append([])
        else:
            segments[-1].append(token)
    return [s for s in segments if s]


def _strip_prefix(tokens: list[str]) -> list[str]:
    words = list(tokens)
    while words and (set(words[0]) <= set("()") or _ENV_ASSIGNMENT.match(words[0]) or words[0] in _PREFIX_WORDS):
        words.pop(0)
    return words


def _basename(word: str) -> str:
    base = re.split(r"[\\/]", word)[-1]
    return re.sub(r"\.(?:exe|cmd|bat)$", "", base, flags=re.IGNORECASE)


# --- output summaries ----------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Outcome:
    ok: bool
    summary: str  # e.g. "42 passed", "3 failed", "build ok", "build failed", "ok", "exit 2"


def summarize(output: object, exit_code: int | None, kind: str) -> Outcome:
    """Summarise a finished command. kind is "test", "build" or "run" (any other command, e.g. from sc-run).

    exit_code None means unknown. Counts found in the output win; failures in the output make the outcome fail even
    with exit code 0."""
    text = clean_output(output)
    failed_exit = exit_code not in (None, 0)
    counts = _counts(text)
    if counts is not None:
        passed, failed = counts
        if failed:
            return Outcome(False, f"{failed} failed")
        if not failed_exit:
            return Outcome(True, f"{passed} passed")
    if failed_exit:
        if kind == "build" and _IDF_BUILD_OK in text:
            return Outcome(False, "flash failed")  # idf.py built the image, a later step (flashing) failed
        return Outcome(False, {"test": "tests failed", "build": "build failed"}.get(kind, f"exit {exit_code}"))
    return Outcome(True, {"test": "tests ok", "build": "build ok"}.get(kind, "ok"))


def clean_output(output: object) -> str:
    if not isinstance(output, str):
        return ""
    return _ANSI.sub("", output[-_TAIL_CHARS:]).replace("\r\n", "\n").replace("\r", "\n")


def exit_code_from_error(error: object) -> int | None:
    """Claude Code's PostToolUseFailure error text for Bash starts with "Exit code N"."""
    match = re.match(r"\s*Exit code (\d+)", error) if isinstance(error, str) else None
    return int(match.group(1)) if match else None


_IDF_BUILD_OK = "Project build complete"  # printed by idf.py build / flash when the build step succeeded


def _counts(text: str) -> tuple[int, int] | None:
    """(passed, failed) from the summary that appears last in the output, or None when no parser matches."""
    found = [r for r in (parser(text) for parser in _PARSERS) if r is not None]
    if not found:
        return None
    _, passed, failed = max(found)
    return passed, failed


def _number(pattern: str, text: str) -> int:
    return sum(int(n) for n in re.findall(pattern, text))


def _pytest(text: str) -> tuple[int, int, int] | None:
    # "==== 3 failed, 42 passed, 1 skipped in 1.23s ====" or, with -q, "42 passed in 0.12s"
    last = None
    for match in re.finditer(r"^[=\s]*(.*\b\d+ (?:passed|failed|errors?)\b.*) in \d+(?:\.\d+)?s\b.*$", text, re.M):
        last = match
    if last is None:
        return None
    line = last.group(1)
    failed = _number(r"\b(\d+) failed\b", line) + _number(r"\b(\d+) errors?\b", line)
    return last.start(), _number(r"\b(\d+) passed\b", line), failed


def _unittest(text: str) -> tuple[int, int, int] | None:
    # "Ran 64 tests in 9.313s" followed by "OK", "OK (skipped=1)" or "FAILED (failures=2, errors=1)"
    last = None
    for match in re.finditer(r"^Ran (\d+) tests? in [\d.]+s\s*\n+\s*(OK|FAILED)\b(.*)$", text, re.M):
        last = match
    if last is None:
        return None
    ran, details = int(last.group(1)), last.group(3)
    failed = _number(r"\bfailures=(\d+)", details) + _number(r"\berrors=(\d+)", details)
    if last.group(2) == "FAILED" and failed == 0:
        failed = 1
    skipped = _number(r"\bskipped=(\d+)", details) + _number(r"\bexpected failures=(\d+)", details)
    return last.start(), max(ran - failed - skipped, 0), failed


def _ctest(text: str) -> tuple[int, int, int] | None:
    # "75% tests passed, 3 tests failed out of 12"
    matches = list(re.finditer(r"\d+% tests passed, (\d+) tests? failed out of (\d+)", text))
    if not matches:
        return None
    last = matches[-1]
    failed, total = int(last.group(1)), int(last.group(2))
    return last.start(), total - failed, failed


def _jest(text: str) -> tuple[int, int, int] | None:
    # jest: "Tests:       1 failed, 41 passed, 42 total"; vitest: "      Tests  1 failed | 41 passed (42)"
    last = None
    for match in re.finditer(r"^\s*Tests:?\s+(\d+ .*)$", text, re.M):
        if re.search(r"\d+ (?:passed|failed)", match.group(1)):
            last = match
    if last is None:
        return None
    line = last.group(1)
    return last.start(), _number(r"(\d+) passed", line), _number(r"(\d+) failed", line)


def _cargo(text: str) -> tuple[int, int, int] | None:
    # one line per test binary: "test result: FAILED. 40 passed; 2 failed; 0 ignored; ..."
    matches = list(re.finditer(r"test result: (?:ok|FAILED)\. (\d+) passed; (\d+) failed", text))
    if not matches:
        return None
    return matches[-1].start(), sum(int(m.group(1)) for m in matches), sum(int(m.group(2)) for m in matches)


def _go(text: str) -> tuple[int, int, int] | None:
    # one line per package: "ok  \texample.com/pkg\t0.012s" or "FAIL\texample.com/pkg\t0.020s"; counts packages
    ok = list(re.finditer(r"^ok\s+\S+\s+(?:[\d.]+s|\(cached\))", text, re.M))
    failed = list(re.finditer(r"^FAIL\s+\S+\s+(?:[\d.]+s|\[[\w ]+ failed\])", text, re.M))
    if not ok and not failed:
        return None
    return max(m.start() for m in ok + failed), len(ok), len(failed)


_PARSERS = (_pytest, _unittest, _ctest, _jest, _cargo, _go)
