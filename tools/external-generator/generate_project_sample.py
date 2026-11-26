#!/usr/bin/env python3
"""Plan A Sample: Generate a valid v-simple-editor (.vseproj) project from Python.

Zero external dependencies (Python 3 standard library only).
Usage:
    python3 generate_project_sample.py output.vseproj [--renderer dummy|edge|quickjs] [--entry path]
"""

import argparse
import json
import urllib.parse
from pathlib import Path


def make_extgen_uri(renderer: str, entry: str = "", params: dict | None = None, fps: float = 30.0) -> str:
    query = [("renderer", renderer)]
    if entry:
        query.append(("entry", str(Path(entry).resolve())))
    if params:
        query.append(("params", json.dumps(params, separators=(",", ":"), ensure_ascii=False)))
    query.append(("fps", f"{fps:g}"))
    return "veditor://extgen?" + urllib.parse.urlencode(query, quote_via=urllib.parse.quote)


def build_sample_project(renderer: str = "dummy", entry: str = "") -> dict:
    intro_uri = make_extgen_uri(
        renderer=renderer,
        entry=entry,
        params={"title": "Wave 25 External Generator", "subtitle": "Generated from Python", "accent": "#f5b82e"},
        fps=30.0,
    )
    outro_uri = make_extgen_uri(
        renderer=renderer,
        entry=entry,
        params={"title": "Subscribe & Thanks!", "subtitle": "Code-driven timeline", "accent": "#4cc9f0"},
        fps=30.0,
    )
    return {
        "version": 2,
        "config": {"width": 1920, "height": 1080, "fps": 30},
        "videoTracks": [
            [
                {
                    "filePath": intro_uri,
                    "displayName": "Intro Generator",
                    "duration": 3.0,
                    "inPoint": 0.0,
                    "outPoint": 3.0,
                    "speed": 1.0,
                    "volume": 1.0,
                    "opacity": 1.0,
                    "videoScale": 1.0,
                    "videoDx": 0.0,
                    "videoDy": 0.0,
                    "rotation2DDegrees": 0.0,
                },
                {
                    "filePath": outro_uri,
                    "displayName": "Outro Generator",
                    "duration": 2.0,
                    "inPoint": 0.0,
                    "outPoint": 2.0,
                    "speed": 1.0,
                    "volume": 1.0,
                    "opacity": 1.0,
                    "videoScale": 1.0,
                    "videoDx": 0.0,
                    "videoDy": 0.0,
                    "rotation2DDegrees": 0.0,
                },
            ]
        ],
        "audioTracks": [[]],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate a sample v-simple-editor .vseproj file.")
    parser.add_argument("output", help="Destination .vseproj path")
    parser.add_argument("--renderer", default="dummy", help="Renderer id (dummy, edge, quickjs, ...)")
    parser.add_argument("--entry", default="", help="Optional HTML/JS entry file path")
    args = parser.parse_args()

    proj = build_sample_project(args.renderer, args.entry)
    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(proj, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Wrote {out_path} ({len(proj['videoTracks'][0])} generator clips)")


if __name__ == "__main__":
    main()
