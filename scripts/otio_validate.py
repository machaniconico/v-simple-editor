#!/usr/bin/env python3
"""Read .otio files with OpenTimelineIO and print what an importer sees.

Usage: python scripts/otio_validate.py <file.otio | folder | pattern> [...]
A folder means its *.otio files and a pattern with * or ? is expanded here
(PowerShell does not expand it for native commands); exit 2 if nothing matches.
Needs the opentimelineio package (put it on PYTHONPATH if it is not installed).
Prints the Timeline, the Stack, each Track and every child (duration(),
range_in_parent, effects) and the markers. Exit code 0 when every file was
read, 1 when a file failed to read, 2 on a usage error.
"""
import glob
import os
import sys

try:
    import opentimelineio as otio
except ImportError as e:
    print("opentimelineio is not importable:", e, file=sys.stderr)
    sys.exit(2)


def describe(path):
    tl = otio.adapters.read_from_file(path)
    print("  ", tl.schema_name() + "." + str(tl.schema_version()),
          "name=", tl.name, "duration=", tl.duration())
    stack = tl.tracks
    print("   Stack", stack.schema_name(), "children=", len(stack),
          "duration=", stack.duration(),
          "markers=", [(m.name, m.marked_range) for m in stack.markers])
    for ti, tr in enumerate(stack):
        print("   Track", ti, tr.name, tr.kind, "enabled=", tr.enabled,
              "duration=", tr.duration())
        for ci, ch in enumerate(tr):
            fx = [(e.schema_name(), getattr(e, "time_scalar", None))
                  for e in getattr(ch, "effects", [])]
            print("     child", ci, ch.schema_name(), repr(ch.name),
                  "source_range=", ch.source_range,
                  "duration()=", ch.duration(),
                  "range_in_parent=", tr.range_of_child_at_index(ci),
                  "effects=", fx)
            if hasattr(ch, "trimmed_range"):
                print("       trimmed_range=", ch.trimmed_range())


def main(argv):
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    files = []
    unmatched = False
    for a in argv:
        if os.path.isdir(a):
            found = sorted(glob.glob(os.path.join(glob.escape(a), "*.otio")))
        elif os.path.isfile(a):
            files.append(a)
            continue
        elif any(c in a for c in "*?["):
            found = sorted(glob.glob(a))
        else:
            files.append(a)
            continue
        if not found:
            print("no .otio file matches:", a, file=sys.stderr)
            unmatched = True
        files += found
    if not files:
        print("no .otio file matches:", " ".join(argv), file=sys.stderr)
        return 2
    print("opentimelineio", otio.__version__)
    rc = 0
    for f in files:
        print("==", f)
        try:
            describe(f)
        except Exception as e:  # report every file, then fail
            print("  READ FAILED:", type(e).__name__, e)
            rc = 1
    return 2 if unmatched else rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
