# Dependency compliance ledger

Last reviewed: 2026-08-11

## AMD Advanced Media Framework (AMF)

| Field | Record |
|---|---|
| Product | AMD Advanced Media Framework headers, consumed by FFmpeg's AMF encoders |
| Version | Pinned vcpkg commit `afeb285c9e2e015292985856b9382ea1d82a4492`: `amd-amf` 1.5.0 |
| Source | https://github.com/GPUOpen-LibrariesAndSDKs/AMF |
| Official build guidance | https://github.com/GPUOpen-LibrariesAndSDKs/AMF/wiki/Build-FFmpeg-with-AMF-Support |
| License | MIT; bundled notice is recorded in `docs/THIRD_PARTY_NOTICES.md` |
| Commercial use / redistribution | The MIT grant permits use, modification, distribution, sublicensing, and sale when its copyright and permission notice are retained |
| Runtime | The AMD display driver supplies the AMF runtime; no service account, API key, network transfer, storage, training use, rate limit, or service fee is involved |
| Output rights | AMF does not claim project media or encoded output rights |
| Standards / patent warning | AMD explicitly grants no license to codec-related standards and says applicable third-party royalties remain the distributor's responsibility |
| Release obligation | Pin and record the vcpkg baseline and resolved AMF version, retain the AMF notice, and reassess codec patent/licensing obligations for every target country and distribution channel |
| Readiness | `HUMAN-REVIEW` for commercial distribution because codec patent and royalty obligations are jurisdiction- and channel-dependent |

The AMF dependency is header-only at build time. Supported AMD hardware and a
compatible driver are still required for hardware encoding at runtime; the
application keeps a software fallback for unsupported or unavailable sessions.

## Alliance for Open Media AV1 codec (libaom)

| Field | Record |
|---|---|
| Product | Alliance for Open Media AV1 reference encoder/decoder, used by FFmpeg as `libaom-av1` |
| Version | Pinned vcpkg commit `afeb285c9e2e015292985856b9382ea1d82a4492`: 3.13.1#2 |
| Source | https://aomedia.googlesource.com/aom |
| Software license | BSD-2-Clause; the full notice is recorded in `docs/THIRD_PARTY_NOTICES.md` |
| Patent terms | Alliance for Open Media Patent License 1.0, including notice/reproduction, necessary-claims availability, and defensive-termination conditions |
| Commercial use / redistribution | Permitted subject to both the BSD notice conditions and the separate patent-license conditions; this ledger is not a non-infringement opinion |
| Runtime / data | Local library only; no account, API key, network transfer, retention, training use, rate limit, or service fee |
| Release obligation | Record the resolved version and feature graph, include the BSD and patent-license texts with binary distributions, and review the patent-license conditions for the distributor |
| Readiness | `HUMAN-REVIEW` for commercial distribution because patent scope and the distributor's Necessary Claims cannot be approved by this automated review |

## FFmpeg build variants

The repository does not currently produce a single FFmpeg license profile:

- `setup.bat` and the Windows CI workflow request the core codec/filter features
  plus `aom` and `amf`, without x264 or x265.
- Both `build.bat` editions additionally request `x264` and `x265`; only the
  Modern edition requests `aom`. The audited local vcpkg metadata enables
  FFmpeg's `gpl` feature for x264/x265 and declares both codec ports
  `GPL-2.0-or-later`.

Release evidence must record the vcpkg baseline, resolved feature graph, SBOM,
license texts, notices, and corresponding-source obligations for the exact
artifact. Commercial/distribution readiness remains `HUMAN-REVIEW`; the AMF
notice's LGPL sentence must not be used to classify an x264/x265-enabled build.
