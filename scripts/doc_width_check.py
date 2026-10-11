#!/usr/bin/env python3
"""Report changed / added lines wider than 120 display columns.

Usage: python3 -I scripts/doc_width_check.py [--base REV] [--max N] FILE...

For each FILE only the lines that `git diff -U0 --inter-hunk-context=0 REV -- FILE` marks as added or
changed are measured (REV defaults to HEAD; an untracked FILE is measured in
full). Width counts East Asian Width W / F characters as 2 columns and
combining marks as 0. Existing lines are never measured, so they are not
re-wrapped. Exit code = 0 when clean, 1 when any line is over-wide, 2 on usage /
git errors.
"""
import re
import subprocess
import sys
import unicodedata

HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def display_width(text):
    width = 0
    for ch in text:
        if unicodedata.combining(ch):
            continue
        width += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return width


def run_git(args):
    return subprocess.run(["git"] + args, capture_output=True, text=True,
                          encoding="utf-8", check=False)


def changed_lines(path, base):
    """Yield (line number, text) for each added / changed line of path."""
    tracked = run_git(["ls-files", "--error-unmatch", "--", path])
    if tracked.returncode not in (0, 1):
        sys.stderr.write(tracked.stderr)
        raise SystemExit(2)
    if tracked.returncode == 1:
        try:
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
        except OSError as err:
            sys.stderr.write("doc_width_check: cannot read %s: %s\n" % (path, err))
            raise SystemExit(2)
        for number, line in enumerate(text.split("\n"), 1):
            yield number, line
        return
    diff = run_git(["diff", "-U0", "--inter-hunk-context=0", "--no-color", base, "--", path])
    if diff.returncode != 0:
        sys.stderr.write(diff.stderr)
        raise SystemExit(2)
    number = 0
    in_hunk = False
    for line in diff.stdout.split("\n"):
        hunk = HUNK.match(line)
        if line.startswith("diff --git"):
            in_hunk = False
        elif hunk:
            in_hunk = True
            number = int(hunk.group(1))
        elif in_hunk and line.startswith(" "):
            number += 1
        elif line.startswith("+") and (in_hunk or not line.startswith("+++")):
            yield number, line[1:]
            number += 1


def main(argv):
    base = "HEAD"
    limit = 120
    files = []
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg in ("--base", "--max"):
            if not args:
                sys.stderr.write("doc_width_check: %s needs a value\n" % arg)
                return 2
            value = args.pop(0)
            if arg == "--base":
                base = value
                continue
            try:
                limit = int(value)
            except ValueError:
                sys.stderr.write("doc_width_check: --max needs an integer: %s\n" % value)
                return 2
        else:
            files.append(arg)
    if not files:
        sys.stderr.write(__doc__)
        return 2
    over = 0
    checked = 0
    for path in files:
        for number, text in changed_lines(path, base):
            checked += 1
            width = display_width(text)
            if width > limit:
                over += 1
                print("%s:%d: %d columns: %s" % (path, number, width, text[:60]))
    print("doc_width_check: %d changed lines checked, %d over %d columns"
          % (checked, over, limit))
    return 1 if over else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
