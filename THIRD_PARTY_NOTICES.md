# Third-party notices

Clipture's own code is licensed under MIT with the Commons Clause License
Condition v1.0 (see [`LICENSE`](LICENSE)). The components below are not
Clipture's code: each remains under its own license, which the Commons Clause
does not change or restrict, and their notices are kept with them.

| Component | Where | License | Notice |
| --- | --- | --- | --- |
| RNNoise | `engine/third_party/rnnoise/` (compiled into `clipture_engine.exe`) | BSD 3-Clause style | [`engine/third_party/rnnoise/COPYING`](engine/third_party/rnnoise/COPYING) |
| nv-codec-headers (NVENC API headers) | `third_party/nv-codec-headers/` | MIT | In each header file |
| Tauri NSIS installer template 2.11.4 | `src-tauri/installer/tauri-2.11.4.nsi` | MIT OR Apache-2.0 | [`src-tauri/installer/LICENSE-MIT`](src-tauri/installer/LICENSE-MIT) |
| DM Serif Display font | `assets/fonts/DMSerifDisplay-Regular.woff2` | SIL Open Font License 1.1 | [`assets/fonts/DMSerifDisplay-OFL.txt`](assets/fonts/DMSerifDisplay-OFL.txt) |
| FFmpeg 9.0.2 "essentials" build (Gyan Doshi) | `ffmpeg.exe`, shipped unmodified beside Clipture and run as a separate program (`scripts/fetch-ffmpeg.cjs`) | GPL-3.0-or-later (the essentials build includes GPL components) | https://www.gyan.dev/ffmpeg/builds/ and https://ffmpeg.org/legal.html |
| Rust crates compiled into `clipture.exe`, npm packages bundled into the UI | `src-tauri/Cargo.lock`, `package-lock.json` | Their own licenses (MIT, Apache-2.0, BSD, ISC and similar; six MPL-2.0 crates, unmodified, source on crates.io; two OFL-1.1 fonts) | `DEPENDENCY-LICENSES.txt`, generated at build time by `scripts/release/collect-dependency-licenses.cjs` |

## FFmpeg

`ffmpeg.exe` is a separate program distributed alongside Clipture, not part of
Clipture's code, and is not covered by Clipture's license. Its GPL terms apply
to it.

The installer places the build's own notices next to Clipture, in
`<install folder>\licenses`:

- `FFmpeg-LICENSE.txt`: the GNU General Public License v3 as shipped in the
  build archive.
- `FFmpeg-README.txt`: the build's README, naming the exact FFmpeg source
  commit and the configuration it was built with.

Both are taken from the pinned, hash-verified archive that `ffmpeg.exe` comes
from (`scripts/fetch-ffmpeg.cjs`). The corresponding source is the FFmpeg
commit named in that README, at https://github.com/FFmpeg/FFmpeg, and the
build's publisher at https://www.gyan.dev/ffmpeg/builds/.

The same folder holds Clipture's `LICENSE.txt` and this file. Runtime updates
replace only Clipture's five runtime files and leave the folder in place.
Updates download `ffmpeg.exe` from a GitHub release; every release carries
the same notice files as release assets beside it, `DEPENDENCY-LICENSES.txt`
included.
