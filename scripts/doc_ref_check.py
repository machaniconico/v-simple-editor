#!/usr/bin/env python3
"""Open every path:line / path:line-line reference of a document and check it.

Usage: python3 -I scripts/doc_ref_check.py [--slack N] [--allow FILE] [--anchors FILE]
           [--git-rev REV] [--stats] [--list-refs] [--self-test] DOC...

References
  full          <path>:<N> or <path>:<N>-<M> (<N>- with no end, <M> < <N>, and a chained <N>-<M>-<digits>,
                also after a continuation, are MALFORMED_REF; <N>-<M>-のように is <N>-<M>). The path token is a run of characters other than
                whitespace, ':' '：' '(' ')' '（' '）' '、' ',' '「' '」' and a backtick, ending in .<1-6 alnum>.
                It resolves to the longest suffix of the token that is a tracked file (repo-relative path or
                basename). A suffix right after an ASCII alphanumeric, '_', '.' or '-' of the token is not a
                candidate (prefix_target.cpp never resolves to target.cpp), nor is one right after a '/', whatever
                precedes that '/' (does-not-exist/target.cpp and 不存在/target.cpp are MISSING_FILE, never
                target.cpp; 入力/出力はtarget.cpp still resolves, its suffix starts after 'は', and a tracked
                Japanese directory as in 資料は素材/note.cpp resolves at the earlier start). A suffix equal to a
                repo-relative path resolves to that file (a file directly under the root included), even when other directories hold
                a file of the same basename. AMBIGUOUS_PATH only when no repo-relative path matches and the suffix
                equals the basename of two or more files. The basename fallback takes only a suffix without '/' (a basename);
                a partial-directory suffix such as a/same.cpp is not resolved. No suffix matches: a known extension
                is MISSING_FILE, an unknown one is not a reference.
  continuation  :<N> or :<N>-<M> right after whitespace, '/', '、', ',', '(', '（' or at line start. It inherits
                the path of the nearest earlier full reference of the same paragraph (a run of non-blank
                lines); none -> MALFORMED_REF.
  Every ':<digits>' token of a document is exactly one of FULL / CONT / EXCLUDED_DIGIT (preceded by an ASCII
  digit, unless its path token resolves to a tracked file such as tool.ps1, which is FULL) / EXCLUDED_QUOTE
  (preceded by " or ') / UNPARSED_COLON_NUMBER. --stats prints the five counts and
  their total and exits 2 when the total differs from an independent count.

Checks per reference, in order: file exists (MISSING_FILE, also for a tracked file that cannot be read;
AMBIGUOUS_PATH), line in range (OUT_OF_RANGE; leading zeros of an endpoint are dropped, so :0003 is line 3, and an
endpoint of more than 9 digits counts as 10**9, so it is OUT_OF_RANGE, never a conversion error; the same holds for
an anchor key), target not blank (EMPTY_TARGET; for a reference with an anchor entry the anchor is compared
first, so a blank target whose anchor is not in the window is LITERAL_NOT_FOUND), literal found in the
range +- slack: (L1) the anchor file entry of path:range, else (L2) backticked literals of the same document
line (a backtick holding a reference of the line is skipped; one holding other ':<digits>' text such as
{"k":1} is a literal), else (L3) identifiers and phrases of the same sentence (LITERAL_NOT_FOUND); none of
them: NO_LITERAL. Anchors are also checked (WEAK_ANCHOR: shorter than
6 chars or on more than N=3 lines of the target; UNUSED_ANCHOR: no reference uses it).

Anchor file: UTF-8 lines "path:range<TAB>literal" (first TAB splits; # starts a comment line).
Allow file: UTF-8 lines "doc:line<TAB>path:range<TAB>reason" (a reference to a past revision; hides reports
only for that resolved reference, except NO_LITERAL; unresolved references use their own token:range) and "unparsed<TAB>doc:line<TAB>token<TAB>reason" (a non-reference ':<digits>' token).
The reason is mandatory: a row with a missing or empty field, or a doc:line that is not ".+:<digits>", is a
usage error (exit 2), as is a path:range that parse_key cannot parse. The reference key uses the exact
--list-refs spelling. A row starting with "unparsed" is never read as a "doc:line" row.
--root DIR reads the file list and the targets from DIR instead of git (used by --self-test).
A document's label (in reports, --list-refs and the allow "doc:line" match) is its repo-relative path, or under
--root its normalized path relative to DIR (./docs/t.md and docs/t.md are both docs/t.md).

Exit code = 0 when clean, 1 when any report was printed, 2 on usage / git / decode errors.
"""
import collections
import contextlib
import io
import os
import posixpath
import re
import shutil
import subprocess
import sys
import tempfile

NUM = "[0-9]+"
KNOWN_EXT = ("cpp", "h", "py", "sh", "txt", "md", "json", "js", "cmake", "qml", "glsl", "frag", "vert")
SPLIT_KINDS = ("FULL", "CONT", "EXCLUDED_DIGIT", "EXCLUDED_QUOTE", "UNPARSED_COLON_NUMBER")
PATH_STOP = ":：()（）、,「」`"
CONT_BEFORE = "/、,(（"
WEAK_N = 3
MIN_ANCHOR_LEN = 6
MAX_LINE_DIGITS = 9
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

TOKEN = re.compile(":" + NUM)
RANGE_TAIL = re.compile(NUM)
EXT_END = re.compile(r"\.([A-Za-z0-9]{1,6})$")
BACKTICK = re.compile(r"(?<!`)(`+)(?!`)(.+?)(?<!`)\1(?!`)")
LIT_SCOPED = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_~][A-Za-z0-9_]*)+")
LIT_ENV = re.compile(r"VEDITOR_[A-Z0-9_]+")
LIT_IDENT = re.compile(r"(?<![A-Za-z0-9_])[A-Za-z_][A-Za-z0-9_]{5,}(?![A-Za-z0-9_])")
LIT_PHRASE = re.compile(r"「([^」]{3,})」")
CASE_MIX = re.compile(r"[a-z][A-Z]|[A-Za-z0-9]_[A-Za-z0-9]")


def is_digit(ch):
    return "0" <= ch <= "9"


def is_path_stop(ch):
    return ch.isspace() or ch in PATH_STOP


class Source:
    """Tracked file list plus a reader, from git (working tree or a revision) or from a directory."""

    def __init__(self, files, reader):
        self.files = sorted(files)
        self._reader = reader
        self._lines = {}
        self._resolved = {}

    def raw(self, path):
        return self._reader(path)

    def lines(self, path):
        if path not in self._lines:
            data = self._reader(path)
            if data is None:
                self._lines[path] = None
            else:
                rows = [row.rstrip("\r") for row in data.decode("utf-8", "replace").split("\n")]
                if rows and rows[-1] == "":
                    rows.pop()
                self._lines[path] = rows
        return self._lines[path]

    def resolve(self, token):
        """Longest suffix of token naming a tracked file: (files, suffix_start). files is [] when none."""
        if token in self._resolved:
            return self._resolved[token]
        found = ([], 0)
        for start in range(len(token)):
            if start and (re.match(r"[A-Za-z0-9_.-]", token[start - 1])):
                continue
            if start and token[start - 1] == "/":
                continue
            suffix = token[start:]
            if not suffix:
                break
            exact = [f for f in self.files if f == suffix]
            if exact:
                found = (exact, start)
                break
            tail = [f for f in self.files if "/" not in suffix and f.endswith("/" + suffix)]
            if tail:
                found = (tail, start)
                break
        self._resolved[token] = found
        return found


def git_source(rev):
    top = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, check=False)
    if top.returncode != 0:
        raise SystemExit(usage_error("not inside a git repository"))
    root = top.stdout.decode("utf-8").strip()
    if rev:
        listing = subprocess.run(["git", "-c", "core.quotepath=off", "ls-tree", "-r", "-z", "--name-only", rev],
                                 capture_output=True, cwd=root, check=False)
    else:
        listing = subprocess.run(["git", "-c", "core.quotepath=off", "ls-files", "-z"],
                                 capture_output=True, cwd=root, check=False)
    if listing.returncode != 0:
        sys.stderr.write(listing.stderr.decode("utf-8", "replace"))
        raise SystemExit(2)
    files = [name.decode("utf-8") for name in listing.stdout.split(b"\0") if name]

    def reader(path):
        if rev:
            shown = subprocess.run(["git", "show", "%s:%s" % (rev, path)], capture_output=True, cwd=root,
                                   check=False)
            return shown.stdout if shown.returncode == 0 else None
        try:
            with open(os.path.join(root, path), "rb") as fh:
                return fh.read()
        except OSError:
            return None

    return Source(files, reader), root


def dir_source(root):
    files = []
    for base, _dirs, names in os.walk(root):
        for name in names:
            files.append(os.path.relpath(os.path.join(base, name), root).replace(os.sep, "/"))

    def reader(path):
        try:
            with open(os.path.join(root, path), "rb") as fh:
                return fh.read()
        except OSError:
            return None

    return Source(files, reader)


def usage_error(message):
    sys.stderr.write("doc_ref_check: %s\n" % message)
    return 2


def read_table(path):
    """Rows (line number, list of TAB fields) of a UTF-8 table file; comments and blanks are skipped."""
    try:
        with open(path, "rb") as fh:
            text = fh.read().decode("utf-8")
    except (OSError, UnicodeDecodeError) as err:
        raise SystemExit(usage_error("cannot read %s: %s" % (path, err)))
    rows = []
    for number, line in enumerate(text.split("\n"), 1):
        line = line.rstrip("\r")
        if line.strip() and not line.startswith("#"):
            rows.append((number, line))
    return rows


def load_allow(path):
    lines = set()
    unparsed = set()
    for number, line in read_table(path):
        fields = line.split("\t")
        where = "%s:%d" % (path, number)
        if fields[0] == "unparsed":
            if len(fields) != 4 or not all(f.strip() for f in fields[1:]) or not re.fullmatch(".+:[0-9]+", fields[1]):
                raise SystemExit(usage_error("%s: unparsed allow row needs 4 non-empty TAB fields "
                                             "(unparsed, doc:line, token, reason)" % where))
            unparsed.add((fields[1], fields[2]))
        else:
            if (len(fields) != 3 or not all(f.strip() for f in fields)
                    or not re.fullmatch(".+:[0-9]+", fields[0]) or parse_key(fields[1]) is None):
                raise SystemExit(usage_error("%s: allow row needs 3 non-empty TAB fields "
                                             "(doc:line, path:range, reason) with a valid path:range" % where))
            lines.add((fields[0], fields[1]))
    return lines, unparsed


def line_number(text):
    """Digits -> line number: leading zeros dropped, more than MAX_LINE_DIGITS digits count as 10**MAX_LINE_DIGITS."""
    digits = text.lstrip("0") or "0"
    return int(digits) if len(digits) <= MAX_LINE_DIGITS else 10 ** MAX_LINE_DIGITS


def parse_key(key):
    """'path:N' / 'path:N-M' -> (path, a, b) or None."""
    path, sep, tail = key.rpartition(":")
    parts = tail.split("-")
    if not sep or not path or len(parts) > 2 or not all(re.fullmatch(NUM, p) for p in parts):
        return None
    return path, line_number(parts[0]), line_number(parts[-1])


def load_anchors(path, source, label):
    """(entries, reports): entries maps (file, a, b) -> [(anchor line, literal)]."""
    entries = collections.defaultdict(list)
    reports = []
    for number, line in read_table(path):
        key, tab, literal = line.partition("\t")
        parsed = parse_key(key)
        if not tab or not literal or parsed is None:
            reports.append((label, number, "MALFORMED_REF", "anchor line needs path:range<TAB>literal"))
            continue
        found, _start = source.resolve(parsed[0])
        if len(found) != 1:
            reports.append((label, number, "UNUSED_ANCHOR", "anchor path does not name one file: " + key))
            continue
        entries[(found[0], parsed[1], parsed[2])].append((number, literal))
    return entries, reports


def sentence_span(line, pos):
    start = line.rfind("。", 0, pos) + 1
    end = line.find("。", pos)
    return start, len(line) if end < 0 else end


def sentence_literals(text):
    found = LIT_SCOPED.findall(text) + LIT_ENV.findall(text)
    for m in LIT_IDENT.finditer(text):
        word = m.group(0)
        if CASE_MIX.search(word) or text[m.end():m.end() + 1] == "(":
            found.append(word)
    found += LIT_PHRASE.findall(text)
    return list(dict.fromkeys(found))


def backtick_literals(line, line_refs):
    """Backticked literals of a line; one that holds a reference of the line (e.g. `target.cpp:3`) is skipped."""
    found = []
    for m in BACKTICK.finditer(line):
        lo, hi = m.span(2)
        if m.group(2).strip() and not any(r["span"][0] < hi and lo < r["span"][1] for r in line_refs):
            found.append(m.group(2).strip())
    return found


def parse_line(source, line, last_full):
    """Classify the ':<digits>' tokens of one line. Returns (refs, other, last_full)."""
    refs = []
    other = []
    for m in TOKEN.finditer(line):
        pos = m.start()
        before = line[pos - 1] if pos else ""
        end = m.end()
        a_text = m.group(0)[1:]
        b_text = a_text
        open_range = False
        ranged = False
        chained = False
        if line[end:end + 1] == "-":
            tail = RANGE_TAIL.match(line, end + 1)
            ranged = True
            if tail:
                b_text = tail.group(0)
                end = tail.end()
                extra = RANGE_TAIL.match(line, end + 1) if line[end:end + 1] == "-" else None
                if extra:
                    # path:1-3-9 is a chained range, never path:1-3 with the -9 ignored
                    chained = True
                    end = extra.end()
            else:
                open_range = True
                end += 1
        start = pos
        while start > 0 and not is_path_stop(line[start - 1]):
            start -= 1
        token = line[start:pos]
        ext = EXT_END.search(token)
        found, offset = source.resolve(token) if ext else ([], 0)
        if before and is_digit(before) and not found:
            other.append(("EXCLUDED_DIGIT", m))
            continue
        if before in ("\"", "'"):
            other.append(("EXCLUDED_QUOTE", m))
            continue
        ref = None
        if ext:
            if found or ext.group(1) in KNOWN_EXT:
                ref = dict(kind="FULL", files=found, token=token[offset:] if found else token,
                           span=(start + (offset if found else 0), end))
        if ref is None and (not before or before.isspace() or before in CONT_BEFORE):
            ref = dict(kind="CONT", span=(pos, end), token="")
        if ref is None:
            other.append(("UNPARSED_COLON_NUMBER", m, token + m.group(0)))
            continue
        a, b = line_number(a_text), line_number(b_text)
        ref.update(pos=pos, a=a, b=b, open=open_range, ranged=ranged, chained=chained)
        if ref["kind"] == "FULL":
            last_full = ref
        else:
            ref["files"] = last_full["files"] if last_full else None
            ref["token"] = last_full["token"] if last_full else ""
        refs.append(ref)
    return refs, other, last_full


def check_ref(source, ref, line, line_refs, anchors, slack, used_keys):
    """Return (kind, detail, literal source) for the first problem of a reference, or (None, ..., level)."""
    spec = "%s:%d%s" % (ref["token"], ref["a"], "-%d" % ref["b"] if ref["ranged"] and not ref["open"] else "")
    if ref["files"] is None:
        return "MALFORMED_REF", "continuation %s has no earlier full reference in its paragraph" % spec, None
    if ref["open"]:
        return "MALFORMED_REF", "open range " + spec + "-", None
    if ref["chained"]:
        return "MALFORMED_REF", "chained range " + spec + "-...", None
    if ref["b"] < ref["a"]:
        return "MALFORMED_REF", "reversed range " + spec, None
    if not ref["files"]:
        return "MISSING_FILE", "no tracked file " + ref["token"], None
    if len(ref["files"]) > 1:
        return "AMBIGUOUS_PATH", "%s matches %s" % (ref["token"], ", ".join(ref["files"][:4])), None
    target = ref["files"][0]
    rows = source.lines(target)
    a, b = ref["a"], ref["b"]
    used_keys.add((target, a, b))
    if rows is None:
        return "MISSING_FILE", "tracked file %s cannot be read" % target, None
    if a < 1 or b > len(rows):
        return "OUT_OF_RANGE", "%s:%d-%d of %d lines" % (target, a, b, len(rows)), None
    blank = all(not row.strip() for row in rows[a - 1:b])
    window = "\n".join(rows[max(a - 1 - slack, 0):b + slack])
    entries = anchors.get((target, a, b))
    if entries:
        # an anchored reference is compared first: a blank target whose anchor is not in the window falls
        # through to LITERAL_NOT_FOUND (NF_L1) below, a blank target whose anchor is in it is EMPTY_TARGET
        level, literals = "L1", [lit for _n, lit in entries]
        if blank and any(lit in window for lit in literals):
            return "EMPTY_TARGET", "%s:%d-%d is blank" % (target, a, b), None
    elif blank:
        return "EMPTY_TARGET", "%s:%d-%d is blank" % (target, a, b), None
    else:
        literals = backtick_literals(line, line_refs)
        level = "L2"
        if not literals:
            level = "L3"
            literals = l3_literals(line, ref, line_refs)
    if not literals:
        return "NO_LITERAL", "%s:%d-%d has no literal to check" % (target, a, b), "NO_LITERAL"
    hits = [lit for lit in literals if lit in window]
    if not hits:
        return "LITERAL_NOT_FOUND", "%s %s not in %s:%d-%d" % (level, literals[:4], target, a, b), "NF_" + level
    weak = not any(len(lit) >= MIN_ANCHOR_LEN and sum(1 for row in rows if lit in row) <= WEAK_N for lit in hits)
    return None, ("WEAK" if weak else ""), level


def l3_literals(line, ref, line_refs):
    start, end = sentence_span(line, ref["pos"])
    text = list(line[start:end])
    for other in line_refs:
        for i in range(other["span"][0], other["span"][1]):
            if start <= i < end:
                text[i - start] = " "
    return sentence_literals("".join(text))



def scan_document(source, label, text, anchors, slack, allow_lines, allow_unparsed, used_keys):
    """Return (reports, listing, stats) of one document."""
    reports = []
    listing = []
    stats = collections.Counter()
    last_full = None
    for number, line in enumerate(text.split("\n"), 1):
        line = line.rstrip("\r")
        if not line.strip():
            last_full = None
            continue
        refs, other, last_full = parse_line(source, line, last_full)
        for entry in other:
            stats[entry[0]] += 1
            if entry[0] == "UNPARSED_COLON_NUMBER" and ("%s:%d" % (label, number), entry[2]) not in allow_unparsed:
                reports.append((label, number, "UNPARSED_COLON_NUMBER", "%s is not a reference" % entry[2]))
        for ref in refs:
            stats[ref["kind"]] += 1
            stats["endpoints_" + ref["kind"]] += 2 if ref["ranged"] and not ref["open"] else 1
            kind, detail, level = check_ref(source, ref, line, refs, anchors, slack, used_keys)
            target = ref["files"][0] if ref["files"] and len(ref["files"]) == 1 else ref["token"]
            spec = "%s:%d%s" % (target, ref["a"],
                                      "-%d" % ref["b"] if ref["ranged"] and not ref["open"] else "")
            if kind:
                if kind == "NO_LITERAL" or ("%s:%d" % (label, number), spec) not in allow_lines:
                    reports.append((label, number, kind, detail))
            elif detail == "WEAK":
                stats["weak_candidates"] += 1
            if level:
                stats["%s_%s" % (ref["kind"], level)] += 1
            where = "-"
            if level in ("L1", "L2", "L3", "NF_L1", "NF_L2", "NF_L3", "NO_LITERAL"):
                where = level
            listing.append("%s:%d: %s %s %s" % (label, number, ref["kind"], spec, where))
    stats["total"] = sum(stats[k] for k in SPLIT_KINDS)
    stats["independent"] = len(re.findall(r":[0-9]+", text))
    return reports, listing, stats


def anchor_reports(source, anchors, used_keys, label):
    reports = []
    for key, entries in anchors.items():
        rows = source.lines(key[0]) or []
        for number, literal in entries:
            count = sum(1 for row in rows if literal in row)
            if len(literal) < MIN_ANCHOR_LEN or count > WEAK_N:
                reports.append((label, number, "WEAK_ANCHOR", "%r is %d chars on %d lines of %s (needs >= %d chars,"
                                " <= %d lines)" % (literal, len(literal), count, key[0], MIN_ANCHOR_LEN, WEAK_N)))
            if key not in used_keys:
                reports.append((label, number, "UNUSED_ANCHOR", "no reference uses %s:%d-%d" % key))
    return reports


def run(source, docs, anchors_path, allow_path, slack, show_stats, list_refs, anchor_label="anchors"):
    allow_lines, allow_unparsed = load_allow(allow_path) if allow_path else (set(), set())
    anchors, reports = ({}, [])
    if anchors_path:
        anchors, reports = load_anchors(anchors_path, source, anchor_label)
    used_keys = set()
    out_stats = []
    for label, text in docs:
        found, listing, stats = scan_document(source, label, text, anchors, slack, allow_lines, allow_unparsed,
                                              used_keys)
        reports += found
        out_stats.append((label, stats))
        if list_refs:
            for row in listing:
                print(row)
    reports += anchor_reports(source, anchors, used_keys, anchor_label)
    return reports, out_stats


def print_stats(out_stats):
    bad = False
    for label, st in out_stats:
        print("== %s: %s total=%d" % (label, " ".join("%s=%d" % (k, st[k]) for k in SPLIT_KINDS), st["total"]))
        if st["total"] != st["independent"]:
            bad = True
            print("   MISMATCH: split total %d != ':[0-9]+' count %d" % (st["total"], st["independent"]))
        for kind in ("FULL", "CONT"):
            print("   %s literal source: L1=%d L2=%d L3=%d NO_LITERAL=%d; not found: L1=%d L2=%d L3=%d" % (
                kind, st[kind + "_L1"], st[kind + "_L2"], st[kind + "_L3"], st[kind + "_NO_LITERAL"],
                st[kind + "_NF_L1"], st[kind + "_NF_L2"], st[kind + "_NF_L3"]))
        print("   line endpoints: full=%d continuation=%d; weak_anchor_candidates(N=%d)=%d" % (
            st["endpoints_FULL"], st["endpoints_CONT"], WEAK_N, st["weak_candidates"]))
    return bad


def main(argv):
    options = dict(slack=0, allow=None, anchors=None, rev=None, root=None, stats=False, list_refs=False,
                   self_test=False, fixture=None)
    docs = []
    args = list(argv)
    while args:
        arg = args.pop(0)
        if arg in ("--slack", "--allow", "--anchors", "--git-rev", "--root", "--fixture"):
            if not args:
                return usage_error("%s needs a value" % arg)
            value = args.pop(0)
            name = {"--git-rev": "rev"}.get(arg, arg[2:])
            if name == "slack":
                if not re.fullmatch(NUM, value) or len(value.lstrip("0")) > MAX_LINE_DIGITS:
                    return usage_error("--slack needs a non-negative integer of at most %d digits: %s"
                                       % (MAX_LINE_DIGITS, value[:40]))
                value = int(value.lstrip("0") or "0")
            options[name] = value
        elif arg in ("--stats", "--list-refs", "--self-test"):
            options[arg[2:].replace("-", "_")] = True
        elif arg.startswith("--"):
            return usage_error("unknown option " + arg)
        else:
            docs.append(arg)
    if options["self_test"]:
        return self_test(options["fixture"] or os.path.join(SCRIPT_DIR, "doc_ref_check_fixture"))
    if not docs:
        sys.stderr.write(__doc__)
        return 2
    if options["root"]:
        source, root = dir_source(options["root"]), options["root"]
        if options["rev"]:
            return usage_error("--git-rev and --root exclude each other")
    else:
        source, root = git_source(options["rev"])
    loaded = []
    for doc in docs:
        if options["rev"]:
            rel = os.path.relpath(os.path.abspath(doc), root).replace(os.sep, "/")
            data = source.raw(rel)
            label = rel
        else:
            path = os.path.join(root, doc) if options["root"] else doc
            if options["root"]:
                label = posixpath.normpath(doc.replace(os.sep, "/"))
            else:
                label = os.path.relpath(os.path.abspath(doc), root).replace(os.sep, "/")
            try:
                with open(path, "rb") as fh:
                    data = fh.read()
            except OSError:
                data = None
        if data is None:
            return usage_error("cannot read document " + doc)
        try:
            loaded.append((label, data.decode("utf-8")))
        except UnicodeDecodeError as err:
            return usage_error("%s is not UTF-8: %s" % (doc, err))
    allow = options["allow"]
    anchors = options["anchors"]
    if allow is None and os.path.exists(os.path.join(SCRIPT_DIR, "doc_ref_allow.txt")):
        allow = os.path.join(SCRIPT_DIR, "doc_ref_allow.txt")
    if anchors is None and os.path.exists(os.path.join(SCRIPT_DIR, "doc_ref_anchors.txt")):
        anchors = os.path.join(SCRIPT_DIR, "doc_ref_anchors.txt")
    label = os.path.basename(anchors) if anchors else "anchors"
    reports, out_stats = run(source, loaded, anchors, allow, options["slack"], options["stats"],
                             options["list_refs"], label)
    for rep in reports:
        print("%s:%d: %s: %s" % rep)
    bad = print_stats(out_stats) if options["stats"] else any(s["total"] != s["independent"] for _l, s in out_stats)
    print("doc_ref_check: %d documents, %d reports" % (len(loaded), len(reports)))
    if bad:
        return 2
    return 1 if reports else 0


def call(argv):
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            code = main(argv)
        except SystemExit as exit_:
            code = exit_.code
    return code, out.getvalue()


def keyset(text):
    """'label:line: KIND: ...' report lines -> set of (label, line, KIND)."""
    found = set()
    for row in text.split("\n"):
        m = re.match(r"^(.+):([0-9]+): ([A-Z_]+): ", row)
        if m:
            found.add((m.group(1), int(m.group(2)), m.group(3)))
    return found


def doc_keys(text, label):
    return {k for k in keyset(text) if k[0] == label}


def self_test(fixture):
    failures = []

    def expect(name, ok, detail=""):
        print("%s: %s%s" % ("PASS" if ok else "FAIL", name, "" if ok else "  " + str(detail)))
        if not ok:
            failures.append(name)

    work = tempfile.mkdtemp(prefix="doc_ref_check_")
    try:
        tree = os.path.join(work, "tree")
        shutil.copytree(os.path.join(fixture, "tree"), tree)
        allow = os.path.join(fixture, "allow.txt")
        anchors = os.path.join(fixture, "anchors.txt")
        names = sorted(os.listdir(os.path.join(tree, "docs")))
        docs = ["docs/" + n for n in names]
        base = ["--root", tree, "--allow", allow, "--anchors", anchors]

        def expected_set(path):
            rows = set()
            for row in open(path, encoding="utf-8").read().split("\n"):
                if row.strip() and not row.startswith("#"):
                    label, line, kind = row.split("\t")
                    rows.add((label, int(line), kind))
            return rows

        want = expected_set(os.path.join(fixture, "expected.txt"))
        code, out = call(base + ["--stats"] + docs)
        got = keyset(out)
        expect("fixture reports equal expected.txt", got == want,
               "missing %s unexpected %s" % (sorted(want - got), sorted(got - want)))
        expect("fixture exits 1 with reports", code == 1, code)
        kinds = {k for _l, _n, k in got}
        every = {"MISSING_FILE", "OUT_OF_RANGE", "EMPTY_TARGET", "LITERAL_NOT_FOUND", "NO_LITERAL", "WEAK_ANCHOR",
                 "UNUSED_ANCHOR", "AMBIGUOUS_PATH", "MALFORMED_REF", "UNPARSED_COLON_NUMBER"}
        expect("all 10 report kinds are exercised", kinds == every, sorted(every ^ kinds))
        expect("no MISMATCH in any --stats total", "MISMATCH" not in out, out[-400:])
        code, out = call(base + ["docs/ok.md", "docs/cont.md", "docs/日本語の文書.md", "docs/fullwidth.md"])
        got = {k for k in keyset(out) if k[0] != "anchors.txt"}
        expect("clean documents (L1 L2 L3 glued Japanese continuation) give only the orphan", got == {
            ("docs/cont.md", 16, "MALFORMED_REF")}, sorted(got))

        # a Japanese file name and Japanese text glued to a path resolve, and the range is read
        jp = "docs/日本語の文書.md"
        code, out = call(base + ["--list-refs", jp])
        expect("Japanese references resolve: ツールバーと機能ガイド.txt:12 L3, target.cpp:3 L3, target.cpp:5 L2",
               all(row in out.splitlines() for row in (
                   jp + ":1: FULL ツールバーと機能ガイド.txt:12 L3", jp + ":2: FULL target.cpp:3 L3",
                   jp + ":3: FULL target.cpp:5 L2")) and not doc_keys(out, jp), out)
        guide = os.path.join(tree, "docs", "日本語の文書.md")
        original = open(guide, encoding="utf-8").read()
        with open(guide, "w", encoding="utf-8") as fh:
            fh.write(original.replace("ツールバーと機能ガイド.txt:12", "ツールバーと機能ガイド.txt:11"))
        code, out = call(base + [jp])
        expect("ツールバーと機能ガイド.txt:11 (shifted by one) is LITERAL_NOT_FOUND",
               doc_keys(out, jp) == {(jp, 1, "LITERAL_NOT_FOUND")}, out)
        with open(guide, "w", encoding="utf-8") as fh:
            fh.write(original)

        # split table
        code, out = call(base + ["--stats", "docs/split.md"])
        expect("split counts FULL=1 (tool.ps1:99) DIGIT=4 QUOTE=1 UNPARSED=2 CONT=0 total=8",
               "FULL=1 CONT=0 EXCLUDED_DIGIT=4 EXCLUDED_QUOTE=1 UNPARSED_COLON_NUMBER=2 total=8" in out, out)
        code, out = call(base + ["--list-refs", "docs/split.md"])
        expect("tool.ps1:99 (a tracked file with a digit-ending extension) is FULL, OUT_OF_RANGE",
               "docs/split.md:6: FULL tool.ps1:99 -" in out
               and ("docs/split.md", 6, "OUT_OF_RANGE") in keyset(out), out)
        code, out = call(["--root", tree, "--stats", "docs/fullwidth.md"])
        expect("fullwidth colon / digits are not tokens (total 0, no report)",
               "total=0" in out and "0 reports" in out, out)
        allow_unparsed = os.path.join(work, "allow_unparsed.txt")
        with open(allow_unparsed, "w", encoding="utf-8") as fh:
            fh.write("unparsed\tdocs/split.md:4\tD:1\tnot a reference\n")
        code, out = call(["--root", tree, "--allow", allow_unparsed, "--anchors", anchors, "docs/split.md"])
        got = keyset(out)
        expect("allow unparsed row removes only D:1", ("docs/split.md", 4, "UNPARSED_COLON_NUMBER") not in got
               and ("docs/split.md", 5, "UNPARSED_COLON_NUMBER") in got, sorted(got))

        # malformed allow rows are usage errors (exit 2), never silently accepted
        bad_rows = (("3 fields, no reason", "unparsed\tdocs/split.md:4\tD:1\n"),
                    ("empty reason", "unparsed\tdocs/split.md:4\tD:1\t\n"),
                    ("doc:line with empty reason", "docs/ok.md:9\t\n"),
                    ("unparsed with 2 fields", "unparsed\tdocs/split.md:4\n"),
                    ("legacy 2 fields", "docs/ok.md:9\tpast revision\n"),
                    ("invalid reference key", "docs/ok.md:9\ttarget.cpp:bad\tpast revision\n"),
                    ("empty reference key", "docs/ok.md:9\t\tpast revision\n"),
                    ("3 fields with empty reason", "docs/ok.md:9\tmissing.cpp:3\t\n"))
        for name, row in bad_rows:
            with open(allow_unparsed, "w", encoding="utf-8") as fh:
                fh.write(row)
            code, out = call(["--root", tree, "--allow", allow_unparsed, "--anchors", anchors, "docs/split.md", "docs/ok.md"])
            expect("allow row (%s) exits 2" % name, code == 2, (code, out))

        # Per-reference allow keys, Unicode tokens, and equal-length backtick spans.
        regression = os.path.join(tree, "docs", "review.md")
        review_allow = os.path.join(work, "review_allow.txt")
        def review(text, allow_text=""):
            with open(regression, "w", encoding="utf-8") as fh:
                fh.write(text)
            with open(review_allow, "w", encoding="utf-8") as fh:
                fh.write(allow_text)
            return call(["--root", tree, "--allow", review_allow, "--list-refs", "docs/review.md"])

        text = "past `betaFunction` target.cpp:3 / :99\n"
        code, out = review(text, "docs/review.md:1\ttarget.cpp:3\tpast revision\n")
        expect("allow for full reference never hides continuation on the same line",
               code == 1 and "OUT_OF_RANGE: target.cpp:99-99" in out, (code, out))
        code, out = review(text, "docs/review.md:1\ttarget.cpp:99\tpast revision\n")
        expect("allow naming the continuation hides precisely that reference",
               code == 0 and "CONT target.cpp:99" in out, (code, out))
        code, out = review("`betaFunction` target.cpp:98 / :99\n",
                           "docs/review.md:1\ttarget.cpp:98\tpast revision\n")
        expect("one of two failing references on a line remains reported",
               code == 1 and out.count("OUT_OF_RANGE:") == 1
               and "OUT_OF_RANGE: target.cpp:99-99" in out, (code, out))
        code, out = review("`gammaHandler` target.cpp:99 / :98-100\n",
                           "docs/review.md:1\ttarget.cpp:98-100\tpast revision\n")
        expect("range allow matches list-refs spelling and leaves full reference reported",
               code == 1 and "CONT target.cpp:98-100" in out and out.count("OUT_OF_RANGE:") == 1
               and "OUT_OF_RANGE: target.cpp:99-99" in out, (code, out))
        code, out = review("甲:1 と 乙:1\n", "unparsed\tdocs/review.md:1\t甲:1\tannotation\n")
        expect("Unicode unparsed allow distinguishes 甲:1 from 乙:1",
               code == 1 and "乙:1 is not a reference" in out
               and "甲:1 is not a reference" not in out, (code, out))
        code, out = review("甲:1 と 乙:1\n", "unparsed\tdocs/review.md:1\t:1\tannotation\n")
        expect("abbreviated :1 allow matches neither Unicode token",
               code == 1 and "甲:1 is not a reference" in out and "乙:1 is not a reference" in out, (code, out))
        code, out = review("x ``a `int` zzNotThere`` target.cpp:2\n")
        expect("double-backtick span is checked whole, not its matching int fragment",
               code == 1 and "LITERAL_NOT_FOUND:" in out and "FULL target.cpp:2 NF_L2" in out, (code, out))
        code, out = review("x ``int alphaFunction() { return 1; }`` target.cpp:2\n")
        expect("double-backtick span present in target passes at L2",
               code == 0 and "FULL target.cpp:2 L2" in out, (code, out))
        code, out = review("``target.cpp:3`` betaFunction\n")
        expect("reference inside double-backtick span is excluded from L2",
               code == 0 and "FULL target.cpp:3 L3" in out, (code, out))
        os.remove(regression)

        # continuation shifted by one line
        def cont_lines():
            code, out = call(base + ["--list-refs", "docs/cont.md"])
            return [ln for ln in out.splitlines() if ln.startswith("docs/cont.md:") and " CONT cont.cpp:" in ln]
        before = cont_lines()
        expect("5 continuations of cont.cpp resolve before the shift",
               [ln.split(":")[1] for ln in before] == ["1", "3", "5", "8", "10"] and all(ln.endswith(" L3") for ln in before), before)
        shifted = os.path.join(tree, "cont.cpp")
        original = open(shifted, encoding="utf-8").read()
        with open(shifted, "w", encoding="utf-8") as fh:
            fh.write("\n" + original)
        code, out = call(base + ["docs/cont.md"])
        got = {k for k in keyset(out) if k[0] == "docs/cont.md"}
        want = {("docs/cont.md", n, "LITERAL_NOT_FOUND") for n in (1, 3, 5, 7, 8, 10, 12, 14)}
        want.add(("docs/cont.md", 16, "MALFORMED_REF"))
        expect("every continuation is reported when its target moves one line", got == want,
               "missing %s unexpected %s" % (sorted(want - got), sorted(got - want)))
        after = cont_lines()
        expect("each of the 5 continuations of cont.cpp is itself reported NF_L3 after the shift",
               [ln.split(":")[1] for ln in after] == ["1", "3", "5", "8", "10"] and all(ln.endswith(" NF_L3") for ln in after), after)
        with open(shifted, "w", encoding="utf-8") as fh:
            fh.write(original)

        # the shape of the README.md:702 case of 0335b53
        real = os.path.join(tree, "docs", "real.md")
        extra = os.path.join(work, "anchors_real.txt")
        with open(real, "w", encoding="utf-8") as fh:
            fh.write("ブラシは残らない。target.md:7。\n")
        with open(extra, "w", encoding="utf-8") as fh:
            fh.write("target.md:12\t元のクリップに戻す\n")
        code, out = call(["--root", tree, "--anchors", extra, "docs/real.md"])
        expect("blank target without anchor is EMPTY_TARGET", doc_keys(out, "docs/real.md") == {("docs/real.md", 1, "EMPTY_TARGET")}, out)
        with open(real, "w", encoding="utf-8") as fh:
            fh.write("ブラシは残らない。target.md:12。\n")
        code, out = call(["--root", tree, "--anchors", extra, "docs/real.md"])
        expect("corrected reference with the anchor is clean", code == 0 and not keyset(out), out)
        target = os.path.join(tree, "target.md")
        original = open(target, encoding="utf-8").read()
        for added, slack, kind in ((3, "0", "LITERAL_NOT_FOUND"), (5, "0", "LITERAL_NOT_FOUND"),
                                   (5, "5", "EMPTY_TARGET")):
            # five lines move the old blank line 7 onto line 12 and the anchor onto line 17: the anchor is
            # compared first (LITERAL_NOT_FOUND); with --slack 5 it is in the window, and the blank line 12
            # is still EMPTY_TARGET, so an anchor within the slack does not hide a blank target
            with open(target, "w", encoding="utf-8") as fh:
                fh.write("x\n" * added + original)
            code, out = call(["--root", tree, "--anchors", extra, "--slack", slack, "docs/real.md"])
            expect("%d lines added to the target, --slack %s, give %s" % (added, slack, kind),
                   doc_keys(out, "docs/real.md") == {("docs/real.md", 1, kind)}, out)
        with open(target, "w", encoding="utf-8") as fh:
            fh.write(original)
        os.remove(real)

        # backticks: other ':<digits>' text is an L2 literal, a backticked reference of the line is not
        code, out = call(base + ["--list-refs", "docs/ok.md", "docs/bad.md"])
        expect("a backticked {\"k\":1} is an L2 literal (found, and not found next to alphaFunction)",
               "docs/ok.md:11: FULL target.cpp:20 L2" in out and "docs/bad.md:13: FULL target.cpp:2 NF_L2" in out, out)
        expect("a backticked reference of the line is not an L2 literal",
               "docs/ok.md:12: FULL target.cpp:3 L3" in out, out)
        expect("chained target.cpp:3-4-5 and the continuation :3-4-5 are MALFORMED_REF (chained range)",
               "docs/bad.md:18: MALFORMED_REF: chained range target.cpp:3-4-" in out
               and "docs/bad.md:19: MALFORMED_REF: chained range target.cpp:3-4-" in out
               and "docs/bad.md:19: FULL target.cpp:3 L3" in out, out)
        expect("target.cpp:3-4-のように (Japanese after the range) resolves to target.cpp:3-4, not MALFORMED_REF",
               "docs/ok.md:14: FULL target.cpp:3-4 L3" in out and not any(k[:2] == ("docs/ok.md", 14)
                                                                         for k in keyset(out)), out)
        expect("不存在/target.cpp:3 is MISSING_FILE, 資料は素材/note.cpp:2 resolves to the tracked Japanese directory",
               "docs/bad.md:17: FULL 素材/note.cpp:2 L3" in out
               and ("docs/bad.md", 16, "MISSING_FILE") in keyset(out)
               and not any(k[:2] == ("docs/bad.md", 17) for k in keyset(out)), out)

        # a tracked file that cannot be read is MISSING_FILE, not OUT_OF_RANGE
        gone = Source(["gone.cpp"], lambda _path: None)
        with contextlib.redirect_stdout(io.StringIO()):
            reports, _stats = run(gone, [("doc.md", "gammaHandler は gone.cpp:1 にある。\n")], None, None, 0, False,
                                  False)
        expect("unreadable tracked target is MISSING_FILE", [r[2] for r in reports] == ["MISSING_FILE"], reports)

        # a document label does not depend on how the path is spelled (allow docs/ok.md:9 hides ./docs/ok.md:9)
        _code, plain = call(base + ["docs/ok.md"])
        _code, dotted = call(base + ["./docs/ok.md"])
        expect("./docs/ok.md gives the same report keys as docs/ok.md (allow row matches)",
               keyset(dotted) == keyset(plain) and ("docs/ok.md", 9, "MISSING_FILE") not in keyset(dotted),
               (sorted(keyset(dotted)), sorted(keyset(plain))))

        # a digit run of more than 4300 digits is classified without a conversion error
        huge = os.path.join(tree, "docs", "huge.md")
        for name, text, want_keys, want_split in (
                ("a long digit run after a digit is EXCLUDED_DIGIT with no report", "ratio 16:" + "9" * 5000 + "\n",
                 set(), "EXCLUDED_DIGIT=1 EXCLUDED_QUOTE=0 UNPARSED_COLON_NUMBER=0 total=1"),
                ("target.cpp: + 5000 digits is one OUT_OF_RANGE", "betaFunction は target.cpp:" + "9" * 5000 + "\n",
                 {("docs/huge.md", 1, "OUT_OF_RANGE")}, "FULL=1 CONT=0 EXCLUDED_DIGIT=0")):
            with open(huge, "w", encoding="utf-8") as fh:
                fh.write(text)
            try:
                code, out = call(["--root", tree, "--stats", "docs/huge.md"])
            except ValueError as err:
                code, out = None, "exception %r" % err
            expect(name, doc_keys(out, "docs/huge.md") == want_keys and want_split in out and "MISMATCH" not in out
                   and code == (1 if want_keys else 0), (code, out[-400:]))
        # leading zeros: target.cpp:0000000003 is line 3, not 10**9
        with open(huge, "w", encoding="utf-8") as fh:
            fh.write("betaFunction は target.cpp:0000000003 にある。\n")
        code, out = call(["--root", tree, "--list-refs", "docs/huge.md"])
        expect("target.cpp:0000000003 checks line 3 (leading zeros dropped)",
               code == 0 and "docs/huge.md:1: FULL target.cpp:3 L3" in out and not keyset(out), (code, out))
        os.remove(huge)

        # more than 4300 digits in --slack or an anchor key: exit 2 / a diagnostic, never a traceback
        try:
            code, out = call(["--root", tree, "--slack", "9" * 5000, "docs/ok.md"])
        except ValueError as err:
            code, out = None, "exception %r" % err
        expect("--slack of 5000 digits exits 2", code == 2, (code, out[-200:]))
        code, out = call(["--root", tree, "--slack", "0000000000003", "docs/ok.md"])
        expect("--slack 0000000000003 (leading zeros) is accepted", code != 2, (code, out[-200:]))
        slack_doc = os.path.join(tree, "docs", "slack.md")
        with open(slack_doc, "w", encoding="utf-8") as fh:
            fh.write("betaFunction は target.cpp:6 にある。\nalphaFunction は target.cpp:6 にある。\n")
        results = {}
        for value in ("0" * 5000 + "3", "3", "2", "4"):
            try:
                results[value] = call(["--root", tree, "--slack", value, "docs/slack.md"])
            except ValueError as err:
                results[value] = (None, "Traceback exception %r" % err)
        code, out = results["0" * 5000 + "3"]
        # betaFunction (line 3) is in target.cpp:6 +- 3, alphaFunction (line 2) only in +- 4
        slack_keys = {value: doc_keys(result[1], "docs/slack.md") for value, result in results.items()}
        nf = lambda *rows: {("docs/slack.md", n, "LITERAL_NOT_FOUND") for n in rows}
        expect("--slack of 5000 zeros then 3 is slack 3 (no exit 2, no traceback, same reports as --slack 3)",
               code == 1 and "Traceback" not in out and slack_keys["0" * 5000 + "3"] == slack_keys["3"] == nf(2)
               and slack_keys["2"] == nf(1, 2) and slack_keys["4"] == set(),
               (code, out[-200:], slack_keys))
        os.remove(slack_doc)
        big_anchor = os.path.join(work, "anchors_big.txt")
        with open(big_anchor, "w", encoding="utf-8") as fh:
            fh.write("target.cpp:%s\tbetaFunction\n" % ("9" * 5000))
        try:
            code, out = call(["--root", tree, "--anchors", big_anchor, "docs/ok.md"])
        except ValueError as err:
            code, out = None, "exception %r" % err
        expect("an anchor key of 5000 digits is an UNUSED_ANCHOR diagnostic on its line",
               code == 1 and ("anchors_big.txt", 1, "UNUSED_ANCHOR") in keyset(out), (code, out[-300:]))

        # --list-refs
        code, out = call(base + ["--list-refs", "docs/cont.md"])
        expect("--list-refs resolves the inherited path",
               "docs/cont.md:1: CONT cont.cpp:4 L3" in out and "docs/cont.md:12: CONT other.cpp:4 L3" in out, out[:500])

        # exits
        code, _out = call([])
        expect("no arguments exit 2", code == 2, code)
        code, _out = call(["--root", tree, "docs/no_such_document.md"])
        expect("missing document exit 2", code == 2, code)
        broken = os.path.join(tree, "docs", "broken.md")
        with open(broken, "wb") as fh:
            fh.write(b"\xff\xfe target.cpp:3\n")
        code, _out = call(["--root", tree, "docs/broken.md"])
        expect("non UTF-8 document exit 2", code == 2, code)
        os.remove(broken)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("doc_ref_check self-test: %d failed" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
