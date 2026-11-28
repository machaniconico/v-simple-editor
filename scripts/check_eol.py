#!/usr/bin/env python3
"""Verify CRLF -> LF compliance per .gitattributes across all tracked git blobs.

Exits 0 when every tracked text file (except *.bat) in HEAD is LF-only
(no CR/CRLF in git index or HEAD blobs), else prints offenders and exits 1.
"""
import subprocess
import sys


def main() -> int:
    out = subprocess.check_output(
        ["git", "-c", "core.quotepath=off", "ls-files", "--eol"]
    ).decode("utf-8", "replace").splitlines()

    offenders = []
    checked = 0
    for line in out:
        parts = line.split("\t")
        if len(parts) != 2:
            continue
        meta, path = parts
        cols = meta.split()
        if not cols or cols[0] == "i/-text":
            continue
        if path.lower().endswith(".bat"):
            continue
        checked += 1
        if cols[0] in ("i/crlf", "i/mixed"):
            offenders.append((path, f"index={cols[0]}"))
            continue
        blob = subprocess.check_output(["git", "show", f"HEAD:{path}"])
        if b"\r\n" in blob:
            offenders.append((path, "HEAD blob contains CRLF"))

    if offenders:
        for path, reason in offenders:
            print(f"EOL_VIOLATION: {path}: {reason}")
        print(f"check_eol: FAILED ({len(offenders)} violation(s) out of {checked} text files)")
        return 1

    print(f"check_eol: PASS ({checked} tracked non-.bat text files verified LF-only)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
