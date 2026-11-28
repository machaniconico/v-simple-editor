#!/usr/bin/env node
// Plan A Sample: Generate a valid v-simple-editor (.vseproj) project from Node.js.
// Zero external dependencies (Node 18+ built-ins only).
// Usage: node generate_project_sample.mjs output.vseproj

import fs from "node:fs";
import path from "node:path";

function makeExtGenUri({ renderer = "dummy", entry = "", params = {}, fps = 30 }) {
  const qs = new URLSearchParams();
  qs.set("renderer", renderer);
  if (entry) qs.set("entry", path.resolve(entry));
  if (params && Object.keys(params).length > 0) qs.set("params", JSON.stringify(params));
  qs.set("fps", String(fps));
  return `veditor://extgen?${qs.toString()}`;
}

const outPath = process.argv[2] || "sample-generated.vseproj";
const project = {
  version: 2,
  config: { width: 1920, height: 1080, fps: 30 },
  videoTracks: [
    [
      {
        filePath: makeExtGenUri({
          renderer: "dummy",
          params: { title: "Node.js Generated Scene", accent: "#4cc9f0" },
          fps: 30,
        }),
        displayName: "Node Scene 1",
        duration: 3.0,
        inPoint: 0.0,
        outPoint: 3.0,
        speed: 1.0,
        volume: 1.0,
        opacity: 1.0,
        videoScale: 1.0,
        videoDx: 0.0,
        videoDy: 0.0,
        rotation2DDegrees: 0.0,
      },
    ],
  ],
  audioTracks: [[]],
};

fs.mkdirSync(path.dirname(path.resolve(outPath)), { recursive: true });
fs.writeFileSync(outPath, JSON.stringify(project, null, 2) + "\n", "utf8");
console.log(`Wrote ${outPath}`);
