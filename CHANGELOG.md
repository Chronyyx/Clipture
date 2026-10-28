# Changelog

## [1.8.0] - 2026-10-28

### Performance

- **Half the capture work while you play:** Windows hands Clipture every frame your screen shows, often three or more for each frame a clip records on a high-refresh monitor. Each one was copied and, with HDR on, tone-mapped on your GPU, and most were then thrown away. Clipture now waits until just before a clip frame is due and processes only the newest screen frame, so frames that could never be recorded cost nothing. On a 210 Hz HDR monitor recording at 60 FPS, that is about half the GPU copies and 12% less engine CPU, which leaves more of your GPU to the game during smokes, molotovs and other heavy moments.
- **Clips stay exactly as fresh:** Every clip frame is the same screen frame it would have been before. In stress tests through smoke and molotov load, fresh frames stayed at 99.9-100% and the longest freeze in smokes dropped from two frames to one.

## [1.7.7] - 2026-10-28

### Updates

- **Remove the old copy whenever you like:** If you said no when Clipture offered to remove its old copy in Program Files, Settings > Storage now has a "Remove old copy" button. It appears only while that copy exists, never as a reminder or badge, and pressing it goes straight to the Windows permission prompt.

### Website

- **No blocked script on clipture.app:** Cloudflare was adding an analytics script to every page that the site's own security policy then blocked. Pages are now served so the network leaves them untouched.

### Development

- **Faster releases:** CI on `main` builds and tests each commit once and keeps the signed build. A release reuses that build instead of compiling and testing the same code again, and starts its own checks while CI is still running. Compiled dependencies are now cached for real, so CI compiles only Clipture's own code: 6.7 minutes instead of 12.9 in the first warm run.
- **Leaner build:** The app library no longer also builds the mobile-only static and dynamic libraries, about 16% off Clipture's own release compile with an identical app.
- **No compiler warnings:** All 31 warnings are gone. Code nothing used is removed, and helpers only tests use are marked as such.

## [1.7.6] - 2026-10-28

### Updates

- **Start with Windows fixed after the move:** After Clipture moved to your user folder, its startup entry could name the app with an internal Windows path form (`\\?\C:\...`) that Windows may not start at sign-in. Clipture now writes the normal path and corrects the entry the next time it starts.
- **The admin prompt is explained first:** Removing the old Program Files copy needs administrator permission once. Clipture now explains that the move makes future updates smooth and asks you first, instead of Windows asking with no warning. Saying no is remembered.
- **The cleanup leaves your firewall alone:** It no longer changes firewall settings, so no PowerShell window opens. Windows asks about the new copy once, the first time friend sharing goes online, and won't ask again after updates because Clipture's path no longer changes.

## [1.7.5] - 2026-10-28

### Updates

- **Updates found while Clipture keeps running:** Clipture checked for updates only when it started, so a copy running since Windows started could miss a release for days. It now checks again every six hours and waits while a clip is saving. Each check is one small request, and an update already downloaded is not fetched again.
- **No leftover installer:** If moving from Program Files to a per-user install was interrupted mid-install, its downloaded installer could stay in the temp folder. The next attempt removes it.

### Development

- **Patches build in seconds:** Release CI built its patch tool as part of the app, recompiling Tauri and Clipture for a tool that needs only zstd. `tools/make-patches` is now a standalone crate, pinned to the zstd release the app decodes with, and builds in about 15 s. A shared fixture fails both crates' tests if the tool and the app ever disagree.
- **Faster releases, tested main:** Release builds never reused their Rust cache, because GitHub lets a tag's workflow read caches only from that tag or `main`, so every release compiled Tauri and every dependency from scratch. A CI workflow now tests every push to `main` and warms the caches releases restore, and a release whose engine sources match reuses the engine CI built and tested.

## [1.7.4] - 2026-10-28

### Updates

- **No more admin prompt after updating:** Each update used to run from a new folder, so Windows Firewall treated it as a new program and asked for administrator rights again the first time friend sharing went online. Clipture now updates its files in place and always runs from the same path, so Windows asks once at most.
- **Tiny downloads:** Updates download a small patch against the files already installed instead of whole files. Going from 1.7.2 to 1.7.3 would have needed 0.46 MB instead of about 30 MB. From three versions back it is still under 2 MB. Anything older, or a fresh install, downloads normally once.
- **Safe if anything goes wrong:** An update is prepared and checked while Clipture keeps recording. Swapping the files on restart takes about 40 ms. The new version counts as installed only after its recording engine is configured and running. A power cut, a crash or a file held open by another program puts the previous version back exactly, and a version that fails to start is not tried again.
- **Faster launches:** Clipture no longer checks every runtime file on each start, which took about 0.7 s.
- **Nothing installed twice:** Old update folders, about 1 GB on a machine that updated through 1.6 and 1.7, are removed, and an update leaves no copies behind once it finishes.
- **Installs without administrator rights:** The installer installs for the current user, in `%LOCALAPPDATA%\Programs\Clipture`, and never asks for elevation. Existing per-user installs keep their folder.
- **Moving from Program Files:** A copy installed for all users moves itself to a per-user install in the background while it keeps recording, keeping settings, clips, shortcuts and start-with-Windows. Removing the old Program Files copy is the only step that needs administrator rights. Clipture offers it once, when you open its window, never during a game, and carries your firewall choice over so Windows does not ask again. Declining leaves both copies working.

### Development

- **ADR 0020:** in-place updates with a write-through journal, recovery from every interruption point, binary patches, and the Program Files migration. Tests inject a power loss before every rename and journal write and check that recovery restores the previous install byte for byte and leaves exactly one copy of each file.
- **Sandbox release test:** test builds compiled with `CLIPTURE_TEST_RELEASE_ORIGIN` take updates from a local mirror, so a full release, update, rollback and migration can run in Windows Sandbox. Release CI refuses builds with it set.
- **Patches in release CI:** releases publish zstd patches from the last three releases with a signed `patches-v1.json`. A failure in that step ships the release without patches.

## [1.7.3] - 2026-10-22

### Recording

- **No more freezes when a game fills VRAM:** With a game using nearly all of a 10 GB card, as Deadlock can, Windows pushed recording frames out of VRAM and clips froze for up to 2 seconds. In stress trials with 8.6 GB held by the game, the longest freeze fell from 1,967 ms to 67 ms, fresh frames rose from 85% to 97%, and missed frames fell from 294 to 60. The game also had more VRAM left for itself.
- **Far less RAM while gaming:** In the same trials the engine used 211 MB of RAM instead of 587 MB. In 1.7.2 the same test reached about 800 MB, and up to 1.2 GB in Deadlock.
- **Freed frames are really freed:** The HDR tone mapper kept up to 16 old frames alive after capture had let them go, so 1.7.2's burst cleanup never gave that memory back.
- **Fewer frames waiting:** A captured frame that a newer one replaces before the next encoder tick is let go at once instead of holding its slot until that tick. The recording is unchanged, frame for frame; during encoder stalls capture holds 6 slots instead of 13-14.
- **HDR prepared at capture:** HDR frames are tone mapped as they are captured, so waiting frames no longer each keep a 28 MB HDR copy. Without VRAM pressure, recording stays at 0 stutter and 100% fresh frames with a third less VRAM.

### Development

- **VRAM-hungry stress trials:** `--vram-mb` makes the stress window hold VRAM like a demanding game, and `--no-solo` skips the window-alone pass for runs that don't need the game-cost comparison, about a third faster.

## [1.7.2] - 2026-10-21

### Recording

- **Burst memory is handed back:** When the encoder falls behind, capture still takes extra frame slots so nothing is lost, but it now frees them a couple of seconds after the burst instead of keeping them until recording stops. At 1440p with Windows HDR each slot is about 44 MB; a stalled encoder grew the pool to 22 slots in testing, nearly 800 MB that now goes back to the game.
- **Smaller encoder start-up:** The encoder sets up 8 output slots instead of 32 and adds more only if a burst needs them, freeing those again once idle. Stress trials of smoke, molotov and spike loads recorded the same as 1.7.1, with no added stutter or lag.

### Development

- **Slot pools in diagnostics:** The engine reports how many capture and encoder slots it holds, the most it held at once and how many it freed, and stress trial reports carry them per second.
- **Spill test on slow CI disks:** The replay spill test allows 60 s to drain instead of 15 s; its linearity check is unchanged.

## [1.7.1] - 2026-10-21

### Library

- **The player uses the whole window:** The clip's title, game and details now sit under the video, and the clip list runs the full height beside it. The list showed two clips at a 1440×900 window and none at 1100×700; it now shows six or more.
- **Trim and mixer fit beside the video:** An open trim editor or mixer takes the list's place at full height, so its keys are no longer cut off. Beside the video the trim editor shows "Start here" and "End here" straight under the overview, one readout for the range and how much it keeps, and zoom as minus, span, plus. On a short window its waveforms shrink so the whole editor stays in view.
- **Toolbar on one row:** "Import videos" and "Select clips" sit with "Save" on the title row, and the game filters stay on one line, scrolling sideways when they overflow.
- **Smooth playback while trimming:** The trim editor followed playback with a loop per redraw and queued every seek, so playback stuttered while trimming. One animation loop now follows playback and at most one seek is in flight.

### Player

- **One time pair:** The display shows where playback is and the scale ends at the clip's length, instead of three times that read as disagreeing.
- **One row of controls on a narrow player:** The level slider folds into the speaker key so the scale keeps its width.
- **Mixer scale:** The faders have one "Quieter / Original / Louder" scale under them, and a quiet level meter no longer draws a groove that looks like a second fader.
- **Edit actions read the same in every theme:** "Trim clip" and "Save to file" look unavailable the same way in every theme until something changes.

### Friends

- **Shared clips read in full:** The list of shared and sent clips runs under the player at full width, so titles are no longer cut to a word beside the video.
- **Friends stay in view:** The people column narrows instead of dropping below the fold at smaller window sizes, and stays in place while the clips scroll.
- **Before a clip plays:** The player shows the sender's colour instead of a black box.
- **Requests in the sidebar:** Friend requests can be declined from the sidebar as well as accepted, and offline friends keep their names in full.
- **Calmer tab:** A clip waiting to be accepted uses the theme's colours, rows have more room, and rename and remove keys stay hidden until a row is in use. The send dialog groups friends who aren't online as Offline and its list grows with the window.

### Settings and themes

- **Readable toggles in Graphite:** A switched-on toggle has a dark knob, so on and off no longer look alike.
- **Labelled update check:** The update key says "Check for updates" instead of showing an icon alone.
- **Page titles line up:** Library, Friends and Settings put their titles on one baseline.
- **Contrast fixes:** the trim range readout in Light, offline friends, Aero's update key and several muted labels.

### Development

- **Faster `npm run dev`:** It builds only the capture engine instead of every engine target, and the engine compiles its sources in parallel (`/MP8`). After a change to an engine header the build before the app opens went from 51 s to 10 s. `build:engine` still builds every target for CI and `ctest`, six projects at a time.
- **No vcpkg hook in engine builds:** The engine vendors its dependencies; a machine-wide `vcpkg integrate install` no longer runs on its targets (and no longer prints `'pwsh.exe' is not recognized` where PowerShell 7 is missing).
- **Release workflow in stages:** The engine is built and tested once and handed on; the Rust tests and the signed release build run side by side, and the release is published only after both pass.

## [1.7.0] - 2026-10-10

### Smooth Clips Through Smokes and Molotovs

- **Game frames from the game itself, desktop kept:** Even whole-monitor capture repeated about a third of the frames in smokes while CS2 ran uncapped above the refresh rate. CS2's own frame times stayed under 10 ms, but with the GPU saturated the compositor skips refreshes, and every whole-desktop capture loses them. While a detected game is in front, covers the monitor and only click-through overlays sit above it, Clipture now records the game window's own frames and draws those overlays (Discord's overlay, FPS counters) on top with their transparency. Alt-tab, any other window on top, or a game window that captures black only switches back to the desktop within 25 ms. Nothing is injected into the game. Uncapped CS2 smokes went from 7.7% to 1.4% of fast-motion frames repeated. `CLIPTURE_HYBRID_GAME_CAPTURE=0` turns it off (ADR 0017).
- **Whole-monitor Windows.Graphics.Capture is the default:** Inside CS2 smokes, Desktop Duplication only received ~35 new desktop images per second while the game presented 150-214 FPS, so 60 FPS clips repeated up to 28 frames per second. Monitor WGC records the same full desktop and received 145-210 frames per second in the same smokes, at no extra game cost. If WGC fails on an output, capture falls back to Desktop Duplication; `CLIPTURE_CAPTURE_BACKEND=dxgi` forces it (ADR 0016).
- **No more frozen frames after a late recorder wake:** Since 1.6.4 a late encoder wake encodes the missed ticks back to back. The first took the newest captured frame and the rest repeated it, so every late wake in a grenade fight left one to three identical frames and pulled newer footage earlier. Every tick more than 2 ms late now takes the frame that was on screen at its own timestamp (`FrameQueue::consumeLatestAtOrBefore`, `CatchUpSelection.hpp`). In the stress trial at 60 FPS, stutter frames with injected 40 ms stalls went from 83-84 to 0-3, and with 65 ms stalls from 152 (83 ms freezes) to 0 (ADR 0015).
- **Capture keeps its turn while the game is busy:** Games register their simulation threads with the Windows multimedia scheduler, the same class acquisition used. Acquisition now runs in its High class (`DisplayPostProcessing`); it needs about 0.5 ms of CPU per frame.
- **Fewer full-screen copies:** Desktop Duplication reports every desktop update (up to the monitor's refresh rate), and every one was copied (FP16 under HDR) even though all but the newest before an encoder tick were thrown away. Acquisition now waits until 2 ms before the next tick and takes the newest image then. Copies per recorded frame fell from about 2.5 to 1.2 with the same freshness.
- **Lower game cost:** Game frame-rate cost while recording at 60 FPS in the smoke phase fell from 16.3% to 11.9% (molly 10.6% to 8.6%).
- **Stress trials without the game:** `clipture_stress_window` (engine tools) and `scripts/performance/run-stress-trial.cjs` record a frame-stamped synthetic workload with scripted smoke, molly and spike phases and count every repeated, missed and late frame per phase. `engine/tools/stress-window/README.md` lists the options that reproduce the CS2 losses.

### Trim Clips In Place

- **Trim mode in the player:** The scissors key on the player's transport opens a trim strip with start and end handles. Drag them, nudge them with the arrow keys (Shift for whole seconds), or press "Start here" / "End here" at the playhead. Playback loops inside the kept part. "Trim clip" asks once more, then deletes everything outside the selection.
- **See the audio while trimming:** The trim editor shows the clip's sound, every played track mixed, drawn on canvas by the player itself (`WaveformOverview`, `WaveformZoom`). The whole clip sits behind the trim strip, and a detail view under it follows the playhead (turning the page at its edge), zooms from 2 to 32 seconds, scrolls by dragging or with the wheel, and shows the kept part as a region whose edges can be dragged; a click there moves the playhead. The waveform (lowest and highest sample per 10 ms, decoded at 8 kHz, in audiowaveform's format) is built as the clip opens, so it is there the moment trim opens, and is not kept. Only the clip's audio packets are read, through its own index (11.5 MB of a 629 MB clip), each track is decoded in parallel on FFmpeg slots of its own (so playback audio never waits), and the tracks are summed in Rust: 0.15 s for a 22 s, 3-track clip and 0.7 s for a 2-minute, 5-track one, where reading the whole file through FFmpeg took 2.9 s.
- **Really deleted, not hidden:** The clip's own file is rewritten in place. Kept frames and audio are packed over the removed ones, the index is rewritten, and the file is cut to its new length. Nothing is re-encoded and no second copy is made. The cut is exact to the frame: the file keeps the frames from the keyframe before the cut, and its edit lists hide them. A 347 MB, 2-minute clip trimmed to 45 s became 128 MB in under a second.
- **Crash safe:** The plan is journaled under `%APPDATA%\Clipture\data\trims` before the first byte moves. A crash or power cut leaves either the untouched clip or, after the next start, the finished trim (ADR 0013).
- **Trim right after saving:** With overlapping clips off, the recorder kept a just-saved clip open until its footage left the replay buffer (up to the clip length plus 5 s), so trimming it said "still using this clip" for about two minutes. It now drops the saved footage at once and copies back only the second of keyframe preroll the next clip needs, which releases the clip within moments.
- **Waits for the recorder:** A clip the recorder is still copying footage from cannot be opened for trimming. The trim waits a few seconds, then says to try again. Imported videos and multi-part clips are not trimmed. Shares of a trimmed clip end. New `trimClip` / `trim_clip`.

### Audio Mixer

- **A level per track in the player:** The sliders key on the transport opens a mixer under the video with every audio track of the clip (system audio, microphone, Discord, each captured app), each with its own fader and mute. Faders move in decibels from −20 dB to +20 dB, with the original level in the middle and silence at the bottom, so equal steps sound like equal changes. They read as perceived loudness (+10 dB sounds about twice as loud): 100% in the middle, 200% three quarters up, 400% at the top. Double-click a fader to return it to the middle. Changes are heard while you move them: playback switches to audio chunks that hold each track apart, and the player balances them itself.
- **Saved into the file:** "Save to file" asks once more, then re-encodes only the changed tracks with their new level (a muted track becomes silence), so every player and upload hears the balance. Video and untouched tracks are not re-encoded or moved. The new audio goes into the old audio's own place in the file, in time order; what is left of the old audio is zeroed. A 2-minute clip with two changed tracks takes about 1.3 s.
- **Crash safe:** The new audio and index are journaled under `%APPDATA%\Clipture\data\mixes` before the clip changes; an interrupted mix finishes at the next start (ADR 0014). Imported videos and multi-part clips are not changed, and shares of a remixed clip end. New `mixClipAudio` / `mix_clip_audio`; `clipPlaybackUrl` reports `audioTrackIndexes`.

### Clips Stored in Recording Order

- **Save in Place clips read front to back:** The replay arena now places each packet at the next free spot after the last one (next fit), wrapping to the start once per lap, instead of in the first hole anywhere in the file. Video frames vary in size, so first fit bounced consecutive frames between the two ends of the arena: a measured 2-minute clip jumped about 290 MB 66 times. Other players and editors had to seek across the whole file, and Clipture needed its linear view. A replay of that clip's packet sizes, with busy and quiet scenes, went from 795 jumps to 4 (two wraps), for the same disk use. Placement is also cheaper: it starts searching at the cursor instead of scanning every hole. Trimmed clips keep this order. Clips recorded before this keep their layout.

### Themes

- **Aero replaces Milate:** A Frutiger Aero theme in the manner of Windows 7: a glass city of the future (a glass tower hung with sky gardens, a stepped tower, a curved white terrace building with planters, street trees, and a glossy plaza with a reflecting pool, lamp banners and people) under a deep blue sky with cumulus, a jet contrail and a flaring sun, with iridescent soap bubbles, frosted glass panes, Windows 7 command buttons and selection highlights, a glossy azure default button, and the player as dark taskbar glass. The scene is drawn as seeded SVG and rendered once to a 280 KB WebP by `node scripts/assets/render-aero-scene.cjs`; the bubbles stay vectors. The glass is painted over it with layered gradients rather than blurred, so nothing is recomputed while the library scrolls or a clip plays. Milate and its font download are gone; settings saved with `"uiTheme": "milate"` load as `"aero"`.
- **Light is machined aluminium:** Bead-blasted silver surfaces with a fine speckle and soft studio light, machined edges, milled keys and recesses, and black OLED readouts. Orange is kept for the record icon and the trim handles. Light drops the Windows title bar and draws close, minimize and maximize as three lights in the window's corner; the top strip drags the window and double-clicks to maximize (`platform/window-presentation.ts`, `app/WindowControls.tsx`). Other themes keep the system frame.
- **Glitten in champagne:** The same machined build as Light in warm anodising, with its sienna marks, serif type and editorial clip layout.
- **Graphite is monochrome:** Black and aluminium with white for what is on and red only for recording; orange no longer fills the editors or selections.
- **The editors follow the theme:** The trim and mixer panels and their waveforms take each theme's colours (they were always dark), and redraw when the theme changes.

### Dependency Updates

- **React 19** (`react`, `react-dom` and their types), with refs typed as nullable where the player passes its video element. **lucide-react 1.x** icons, **concurrently 10** (legacy dev script, Node 22+), **base64 0.23**, **minisign 0.10** (test signing only) and **actions/setup-node v7** in the release and site workflows.
- Held back: Tauri 2.12 with its plugins and JS packages (needs a launch test of the real app first), `sha2` 0.11 (would rewrite the update hash checks for no gain), `auto-launch` 0.6 (must stay on the version `tauri-plugin-autostart` uses so the installer writes the same startup entry) and `@vitejs/plugin-react` 6 (requires Vite 8).

## [1.6.6] - 2026-10-03

### Removed Clips Stop Showing as Watched

- **Deleting a clip mid-stream now reaches the sender:** When a friend trashed a clip they had accepted, nothing told your Clipture, so it kept showing them watching (or the last progress) and the share stayed open. Their Clipture now stops the stream right away and sends `answer` with `removed: true` (queued until you are reachable). Your side closes the share, stops serving it, and shows "<name> removed it". Sending it again asks them again. Older senders read this as a decline, which closes the share too.

## [1.6.5] - 2026-10-02

### Friend Nicknames, Smooth Playback & Instant Sends

- **Nicknames only you see:** Any friend or friend request can be renamed from the Friends tab (pencil on the row). The nickname replaces the name they chose everywhere in your Clipture (friends list, sidebar, send dialog, incoming clips, notices), is never sent to them, and outlasts any name they pick later. "Use their name" clears it. New `setFriendNickname` / `sharing_set_nickname`; friends carry `nickname` in `state.json`.
- **No more freeze about a second into a clip:** Save in place turns the replay buffer itself into the clip, so frames sit wherever the buffer had room (in a measured 2-minute clip, consecutive frames jumped hundreds of MB about 850 times). The player had to jump across the whole file and froze at about 0.7 s while its cache filled. Such clips are now played through a linear view: the same file with only the index's chunk offsets rewritten and every chunk served in playing order, read from the original on request (`media/linear_view.rs`). Nothing is copied or written, and clips that are already in order are served as before. In a headless Edge replay of that clip, playback start went from a 0.8 s freeze, about 375 range requests and 1.5 GB read to no freeze, 31 requests and 124 MB. All 35,413 packets are byte-identical to the original.
- **Sending a clip to a friend starts right away:** Out-of-order clips are sent through the same view instead of a remuxed FFmpeg copy under `<saveFolder>\.clipture-sharing`, so there is no remux wait and no extra 700 MB on disk. The offer's size and digest are the view's, and the friend keeps a normal time-ordered MP4. Shares made by older builds keep their copies (outgoing shares gain `linear` in `state.json`).

## [1.6.4] - 2026-10-02

### Smooth Clips Under Game Load

- **Smooth clips under game load:** Since the September pacing change, a recorder wake that ran even slightly late skipped that frame, leaving gaps that made clips choppy in games. A wake up to 70 ms late now catches up: the missed frames are encoded back to back, each on its own evenly spaced timestamp. Only longer stalls (capture transitions) skip ahead, so video still cannot drift behind audio (`EncoderCadence`).

## [1.6.3] - 2026-10-02

### Reliable Update Installs

- **Installing an update no longer fails at random while recording:** Install was refused whenever the frame queue showed a momentary "elevated" reading, which happens in normal recording. Installing is always an explicit click and stops capture anyway, so only an active clip save now holds it back (still enforced atomically by the installation reservation). Update downloads keep their capture-aware pacing.
- **Friend removals and presence are more robust:** A removed friend's retried hello can no longer turn into a new friend request: the owed goodbye stays owed until they acknowledge it, and it is sent the moment they reappear. Replies to someone who is not a friend now wait to be delivered before the connection closes (closing first could discard them). A node that is shutting sharing off sends nothing after its "going offline" notice, so friends no longer see it come straight back.
- **Readable error notices:** Update errors show the reason as a sentence instead of the diagnostic "Tauri capability ... failed through ..." form (`userMessage` in `platform/hostError.ts`).

## [1.6.2] - 2026-10-02

### Clip Requests, Live Send Status & Timed Access

- **Friends accept clips first:** A clip a friend sends now waits in Friends with Accept and Decline. The Friends badge counts it, a notice appears anywhere in the app, and a sound plays even with the window closed. Nothing streams or downloads until it is accepted. Peer message `answer` and the store's `answers` queue carry the decision back (ADR 0011).
- **Live send status:** After sending, the dialog follows the clip: preparing, waiting for the friend, declined, accepted, how much they have watched or downloaded with speed, "Connection lost" if the connection closes mid-send, and a check once they have all of it. Progress is counted on the sender as bytes leave. The Friends tab gains a Sent list that reopens the same view, with Stop sharing.
- **Sound cues:** Short synthesized cues for an incoming clip, accepted, declined, finished and interrupted sends. No sound files are bundled.
- **Send dialog:** The clip's thumbnail leads, friends are grouped Online and Away, and the send button has a fixed place in the footer. Choosing a friend only selects them, so a stray click can never send.
- **A saved clip is closed:** Once a friend's "Add to library" copy passes its integrity check, their Clipture confirms it (`answer` with `kept`). Your PC then stops serving that clip for good, by stream or download, and deletes its stream copy. The check mark now means this confirmed save. Sending it again on purpose asks them again.
- **15-minute window:** After accepting, a friend has 15 minutes to watch a clip or start adding it to their library. Then your PC stops serving it. A download that began in time may still finish. Both sides show the time left and a "Time's up" state, and sending it again gives another 15 minutes.
- **Removing an offline friend sticks:** A friend removed while offline used to keep you on their list. The removal is now kept until it reaches them, and when they come back online and check in, Clipture answers that you are no longer friends, so their side drops you too (peer reply `notFriends`, store `goodbyes`).
- **Stream cache sweep:** Startup only deletes stale `clipture-stream-*` temp files, so it can no longer remove a stream file that another session is still writing.
- **Friends tab:** Each friend has a consistent colour on their avatar and the clips they send. Your name, invite link, requests and friends share one panel, and adding by code folds away. The library's send action is now a labelled button under the clip details.

## [1.6.1] - 2026-10-01

### Version Label & Update Status

- **Version in the sidebar:** The running version is shown at the bottom of the sidebar. It is taken from `package.json` at build time, so it always matches the executable the interface ships in, and it stays pinned while a long friends list scrolls.
- **"Updates off" for local builds:** Builds that cannot update themselves (local `build`/`dist:win` output, development and test builds) now show "Updates off" in the title bar, with the reason on hover, instead of an update check that can never find anything. `UpdateState` gains an optional `disabled` flag; release builds are unchanged.
- **First runtime update:** 1.6.0 installs receive this release through the signed component runtime path (ADR 0009) instead of the installer.

## [1.6.0] - 2026-10-01

### Friend Sharing, Smaller Clips & Signed Runtime Updates

#### Friend-to-friend clip sharing (ADR 0011)

- **Peer-to-peer sharing over iroh:** New opt-in Friends tab. Friends are added by invite link (`clipture.app/invite`) and clips go directly between the two PCs over QUIC, with no Clipture server in the path. LAN peers are found over mDNS and connect directly; a relay is used only when no direct path exists, and the UI says when it is.
- **Click-to-play streaming:** A shared clip streams only after Play is pressed. Up to four fetch runways follow the playhead, fill the clip in playback order from its sample tables, and skip zero-filled ranges on the wire (sparse range replies). The stream is cached in a sparse temporary file, so seeking back is instant and every audio track can be mixed once the clip has arrived.
- **Stream-ready copies:** Sharing makes a lossless, verified FFmpeg remux (`+faststart`, playback-ordered samples, no padding) under `<saveFolder>\.clipture-sharing`, capped at 10 GB / 30 days and removed with the share. Originals are never modified. A 719 MB clip over a 56 Mbit/s upload now shows its first frame in 1.6 s instead of 8.9 s.
- **Resumable "Add to library":** Downloads run at the full speed of both connections, resume from a verified partial file after a drop (backoff of 1–30 s), show speed and relay state, and can be cancelled.
- **All audio tracks:** Streams and library copies keep every track; streamed playback mixes the selected tracks through FFmpeg.

#### Recording and saving

- **Capped-quality rate control (ADR 0012):** NVENC now encodes VBR at CQ 21, using the configured bitrate as a one-second VBV ceiling, instead of constant bitrate. Measured on 1440p120 footage: 24–32% of the previous size at VMAF within 0.15, and under 1% more encoder time. Constant bitrate remains the automatic fallback for GPUs or drivers that reject it.
- **No more padded clips:** Save trims unused arena space off the end of an in-place clip and releases free slots inside it as sparse ranges, instead of writing them out as zeros. Up to half of a clip could previously be padding.
- **Exact frame rate:** A save now waits (bounded to 250 ms) for frames still inside the encoder, so clips end on real frames rather than holding the last one. Clips report exactly their capture rate (60, not 59.97).
- **120 FPS capture:** Added as an experimental recording target.
- **Overlapping in-place saves:** New default `saveInPlaceOverlap`. Footage retained after a save is copied back into the live arena in the background at low I/O priority, so consecutive saves can overlap like the legacy RAM path.
- **Fix clips:** Settings can now losslessly repair clips with a poor sample layout or zero padding, after verification and without touching clips that do not need it.
- **Replay-buffer cleanup:** Abandoned `.clipture-replay` buffers left by crashes or forced exits are removed on start.
- **Default app tracks:** The default browser and Discord are added once as separate audio tracks and never re-added after removal.
- **Fresh NV12 conversion (ADR 0010):** Opt-in experiment that converts the selected frame straight into the encoder's input surface.

#### Updates and reliability

- **Signed component runtimes (ADR 0009):** Updates stage a Minisign-signed runtime from the GitHub release, reusing unchanged files and 1 MiB blocks, and activate on the next launch or on Apply now. No installer or elevation is needed.
- **Engine lifetime:** The capture engine is tied to the controller with a kill-on-close job object, so it can no longer be orphaned and block a reinstall.
- **UI stability:** Media replies over the UI pipe's 4 MiB limit fail that request instead of closing the window, and closing the window is always delivered.
- **Crash log:** Panics and unexpected UI-worker exits are written to `crash.log` in the data folder.

#### Interface

- **Settings redesign:** Recording, Storage, Audio and Notification sections, a hotkey field, and wording that says what each setting does ("Maximum bitrate").
- **Library:** Loading skeletons, lazy thumbnail loading, and a recorder status panel.
- **Themes:** New Maid café and Halloween themes.

#### Build and release

- **Pinned FFmpeg:** FFmpeg 9.0.2 is downloaded once and verified against pinned SHA-256 hashes.
- **Optional Authenticode signing:** Configurable through `WINDOWS_SIGN_COMMAND`.
- **Hardened workflows:** Actions are pinned to commit SHAs and maintained by Dependabot.
- **clipture.app:** The site is rendered and deployed with each release.

## [1.5.4] - 2026-09-11

### Audio Mixer Quantization & 100 Hz Buzz Elimination

- **Nearest-Sample Grid Quantization:** Replaced truncating integer division in `AudioReplayCoordinator` with a dedicated nearest-sample rounding implementation (`PcmBlockMixer.hpp`). Truncation toward zero previously placed adjacent packets (-479.995 frames and +0.005 frames) into the same output frame index zero, causing an artifactual doubled sample every 480 frames and pumping the peak limiter at the 100 Hz mix-block cadence.
- **Consistent Half-Frame Tie Breaking:** Standardized boundary tie breaking toward the later frame across negative and positive fractional offsets, ensuring continuous sample flow across 8 kHz, 44.1 kHz, 48 kHz, and 96 kHz streams even after sub-millisecond clock adjustments.
- **Audio Integration & Phase Sweep Test Coverage:** Added `clipture_pcm_block_mixer` unit tests verifying boundary phase continuity across sample sweeps and multi-track summation, alongside `testAudioMixerClockPhaseDoesNotChangeAac` validating that fractional clock phase corrections do not alter AAC stream encodings.

## [1.5.3] - 2026-09-10

### WASAPI Loopback Pacing & Audio Sync Stabilization

- **Loopback Silence Scheduling & Grace Period:** Refactored system and process-level WASAPI loopback capture fallback (`LoopbackSilencePolicy.hpp`). An empty poll (`GetNextPacketSize() == 0`) is no longer prematurely treated as silence; synthetic silence packets are only fabricated after a bounded 200 ms delivery grace period (two 100 ms WASAPI client periods).
- **Audio Timestamp Alignment & Drift Prevention:** Prevents premature silence frames from shifting incoming real audio packets forward in the forward-only timestamp aligner, eliminating cumulative audio desync, artificial gaps, stutter, and intermittent buzzing artifacts on resume.
- **Dedicated Loopback Silence Test Suite:** Integrated `clipture_loopback_silence` regression suite into CTest covering continuous streams with empty polls, 5–100 ms variable packets, jittered/batched delivery, idle-source advancement, and audio resumption.

## [1.5.2] - 2026-09-10

### In-Place Replay Saving & Zero-Copy MP4 Persistence

- **In-Place Replay Persistence & Zero-Copy Finalization:** Replaces disk/memory duplicate muxing with a dedicated packet arena (`replay/InPlacePacketArchive`). Video and normalized AAC audio samples are written directly into private `.clipture-replay/*.recording` arenas inside the user's save folder, finalizing in place via atomic MP4 atom construction (`ftyp`/`free`/64-bit `mdat`/`moov`) with zero media reads or writes during save.
- **Rolling Disk Arena & Bounded Storage:** Implements packet lease protection and hard arena caps, reusing retired unleased slots and zeroing free blocks upon detachment to keep disk usage strictly bounded.
- **Consume-on-Save Window Progression:** Automatically advances the consumed media boundary on save success so rapid successive saves (e.g. 120s followed by 10s) produce only the newly captured delta while retaining necessary decoder preroll.
- **Native Error Resilience & Windows Path Escaping:** Correctly escapes Windows paths and quotes in native protocol error replies, preventing engine communication timeouts, and guarantees unique sequential filenames for rapid saves.
- **Shell Explorer Selection & Settings Integration:** Added native Windows `SHOpenFolderAndSelectItems` explorer selection support and exposed the new `saveInPlace` preference (default on) across frontend settings and host adapters.

## [1.5.1] - 2026-09-09

### Themed Installer, Player Lifetime & Visual Identity

- **Themed NSIS Installer & Setup Customization:** Replaced generic installer chrome with a customized silver and graphite field-recorder visual theme, featuring high-resolution 4x bitmap artwork, GDI halftone scaling, custom typography, and an interactive startup registration page.
- **Clean Electron Startup Migration:** Automatically cleans up legacy Electron Run registry entries (`electron.app.Clipture` and `app.clipture.desktop`) upon installation or launch while synchronizing the canonical `startOnLogin` startup preference.
- **Player Resource Lifetime & Memory Optimization:** Implemented strict unmount disposal for HTML5 video elements, resetting decoders and releasing buffer memory when navigating away from the player. Web Audio graphs are now allocated lazily only for volume boost or mixed audio tracks, and audio polling timers suspend during pause or backgrounding.
- **Instant Window Presentation:** The main UI window initializes hidden with a native dark background and empty caption, revealing only after authoritative settings, appearance styles, and themes have fully loaded—eliminating startup flash.
- **New Brand Identity & High-Resolution Icon:** Introduced a transparent master render for the hardware recorder body and updated high-resolution application icons (`icon.ico`, `clipture-logo-ui.png`), along with targeted Windows shortcut notifications on installation (`icon-refresh.nsh`).
- **Diagnostics Refresh Resilience:** Hardened diagnostics snapshot recovery, avoiding stale telemetry while handling background save timeouts gracefully.

## [1.5.0] - 2026-09-08

### Native Tauri 2 Architecture & Ultra-Low Footprint

- **Migration from Electron to Tauri 2:** Complete transition of the desktop host to Tauri 2 with native Windows integration, slashing idle resident memory from ~150 MB down to ~4.2 MB.
- **Disposable UI Process (ADR 0004):** The user interface window runs in an isolated worker process that completely exits when closed, eliminating WebView2 process and handle accumulation while background capture continues uninterrupted.
- **Native Background Capture Continuity:** High-performance C++ NVENC capture engine, WASAPI audio loopback, global hotkeys, and clip saving run entirely in the resident background controller without requiring any open web interface.
- **Native Windows Notification Overlay:** Pixel-perfect notification overlay rebuilt with Win32 GDI, layered window opacity, sharp border-flush geometry, and click-through support without web runtime overhead.
- **Seamless Electron Crossgrade & Signed Updates:** NSIS installer seamlessly upgrades existing Electron installations while preserving 100% of user data, clips, and settings in `%APPDATA%\Clipture\data`.

## [1.4.2] - 2026-09-01

### Black Screen Capture Fix & Audio Buzz Elimination

- **WGC HDR Tonemapping & Safe Subresource Region Copying:** Restored safe `CopySubresourceRegion` texture bounds handling and tonemapper processing for Windows Graphics Capture (WGC), eliminating black screen captures when recording HDR displays or window surfaces with dimensional padding and borders.
- **DXGI Output Format Hardening:** Updated DXGI Desktop Duplication format validation to support SDR `DXGI_FORMAT_B8G8R8A8_UNORM` even on HDR-enabled displays, and dynamically detected capture formats to avoid false capture failures.
- **Capture Frame Lifecycle & COM Pointer Isolation:** Scoped captured frame handles and presentation timestamps strictly to the execution lifecycle of `EncoderWorker::run()`, preventing stale texture pointers from previous capture sessions from leaking into new recordings.
- **Continuous Audio Loopback Silence Filling:** Restored continuous silent packet generation during periods of silence in WASAPI loopback capture (both system and game process loopback). This preserves a strictly continuous, monotonic audio PTS timeline, completely eliminating the timestamp stalls that previously forced the live AAC encoder to restart with audible buzzing, clicking, and crackling artifacts.

## [1.4.1] - 2026-08-23

### Asymmetric Cadence Smoothing & Rock-Solid 60 FPS Pacing

- **Asymmetric Framerate Transition Easing:** Implemented a "Fast Attack, Smooth Decay" exponential moving average filter for live framerate and cadence smoothing. When emerging from low-motion zones (such as smoke or static scenes), framerate metrics attack instantly ($\alpha = 1.0$, 0ms delay) to maintain responsive visual telemetry, while entering low-motion scenes tapers gracefully along a smooth exponential decay curve ($\tau = 0.7\text{s}$) rather than dropping off abruptly.
- **Constant Frame Rate (CFR) Duplication Control:** Added an explicit `kEnableFrameDuplication` policy toggle, ensuring mathematical CFR frame progression and timeline synchronization across all media players and video editors without stuttering.
- **FPS Presets Streamlined to 60 FPS:** Focused core capture options on ultra-stable 24, 30, and 60 FPS presets, removing experimental high-framerate selections to guarantee rock-solid recording performance across all hardware configurations.
- **Hardware Pacing & Latency Hardening:** Maintained 0.00ms scheduler wake lateness with sub-millisecond timer precision, preventing DWM presentation jitter during demanding game loads.

## [1.4.0] - 2026-08-18

### Smooth 60 FPS Capture Engine & OBS-Aligned Frame Pacing

- **OBS Studio Alignment:** Re-architected desktop duplication and frame delivery to match OBS Studio's direct on-tick sampling model, eliminating thread-hop pre-arm races, queue latency, and 3:2 cadence judder.
- **Unified D3D11 Pipeline:** Unified the DirectX 11 capture, color conversion, and NVENC encoding devices (`kPreferSharedCaptureDevice = true`), completely eliminating cross-device KeyedMutex `AcquireSync`/`ReleaseSync` synchronization stalls and GPU latency spikes.
- **WDDM Realtime GPU Priority & Video Acceleration:** Elevated capture and encoder D3D11 thread priority to maximum realtime (`SetGPUThreadPriority(7)`) and enabled `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`, guaranteeing 60 FPS capture execution even under 99–100% GPU loads during in-game smoke and particle bursts.
- **Precision Hybrid Scheduler Clock:** Integrated high-resolution hybrid timer (`PrecisionTimer`) combining Windows `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, `timeBeginPeriod(1)`, and sub-millisecond CPU pause/yield spin loops to eliminate standard Windows sleep latency jitter.
- **NVENC VBV & 2.0s GOP Alignment:** Configured 1-second VBV buffer provisioning (`vbvBufferSize = averageBitRate`) to absorb high-entropy particle bursts without encoder stalling, and standardized keyframe intervals to 2.0-second GOP cadence without manual IDR collision pulses.
- **Constant Frame Rate (CFR) MP4 Muxer:** Quantized video sample durations in `Mp4Muxer` to exact integer multiples of `10,000,000 / fps`, eliminating duration variance in the MP4 `stts` atom for flawless 60 Hz playback across all media players.
- **Diagnostics & Telemetry:** Added full frame drop diagnostics and telemetry reporting granular capture and encoder drop metrics.

## [1.3.3] - 2026-08-11

### Library Bugfix

- Restore the clip grid beneath the active video player in Graphite and other standard themes while keeping Glitten's editorial rail-only layout.
- Highlight the currently playing clip in the restored grid.

## [1.3.2] - 2026-08-11

### Audio Reliability

- Preserve format-aware microphone decoding while restoring the S16-scaled input expected by RNNoise, preventing speech-triggered hiss and isolation artifacts from high-resolution endpoint noise.
- Apply the stabilized microphone path consistently in both dedicated and compatibility capture workers.

### Library And Theme Fixes

- Keep Glitten in its editorial player-and-rail layout while selecting clips instead of switching back to the standard grid.
- Add selection controls directly to the Glitten clip rail, including Select all support, without treating the active video player as another selected item.
- Keep update scanning and preparation indicators visible against light themes by using theme-aware accent colors.

## [1.3.1] - 2026-08-11

### Hotkeys And Reliability

- Replace Electron's OS-registered save shortcut with a native Windows Raw Input listener that remains available in background and fullscreen applications without claiming the key combination globally.
- Keep the existing configurable keybind workflow, automatically re-arm it after native engine restarts, suppress repeat-triggered saves, and include listener status and trigger history in exported diagnostics.

### Playback And Library

- Add an editorial Glitten library view that plays clips in place and keeps a scrollable related-clips rail beside the video.
- Add Spacebar play and pause controls and automatically resume playback after seeking into a range-buffered section that still needs to load.
- Keep the embedded player at a stable 16:9 ratio while resizing, prevent native video dimensions from widening the layout, and make the related-clips rail scroll without stretching or letterboxing the video.

### Audio Reliability

- Convert microphone input from its declared PCM container, valid-bit depth, and floating-point format directly into processing samples before gating or RNNoise isolation.
- Ignore unused low container bits so padded high-resolution microphone formats do not introduce hiss or static into saved clips.

## [1.3.0] - 2026-08-09

### New Features

- Add the Glitten and Milate visual themes with coordinated colors, typography, previews, and native window appearance.
- Add a shared theme-font workflow that opens the personal-use download page without bundling restricted font files.
- Detect installed Glitten and Dh Milate fonts at runtime and activate them without restarting Clipture after installation.
- Add automatic font refresh when Clipture regains focus, plus a manual Refresh font control for systems that update their font catalog slowly.

### UI And Reliability

- Keep the bundled fallback typefaces available when a personal font is not installed.
- Include the exact Milate demo family name in local-font detection so the downloaded personal-use font is recognized reliably.
- Consolidate Glitten and Milate font IPC, renderer loading, and status handling into one allowlisted implementation.

## [1.2.4] - 2026-08-08

### Replay Engine & Memory Management

- Add resident payload budgeting and hybrid disk spilling to `ReplaySegmentStore`, automatically spilling older packet payloads to managed disk segments when RAM limits are reached while keeping packet index structures resident.
- Report resident bytes, resident payload budget, resident packet counts, and disk-backed packet counts in engine diagnostics and telemetry.
- Adjust tonemapper view cache limit and staging window allocation to scale smoothly based on target disk seek penalties.

### Automatic Updates

- Implement capture-aware update transfer policy (`CaptureAwareUpdater` and `UpdateTransferPolicy`) that automatically throttles or pauses background update downloads during active capture or high-load clip saving.
- Include unit coverage for background download state transitions and capture pressure detection.

### UI & Customization

- Add full theme support (`Graphite`, `Light`, and `Custom` theme options) with customizable primary and accent color pickers in Settings.
- Synchronize Electron window background and native title bar controls with active theme tokens.
- Add system tray double-click interaction to restore and focus the application window.

### Diagnostics & Validation

- Expand save timing probes, log paths (`save-timing.log`), and diagnostic exports (`reportVersion: 2`) with post-save performance tracing.
- Verified TypeScript typecheck, native packet architecture tests, update transfer suite, and clean release notes extraction.

## [1.2.3] - 2026-08-02

### Capture Performance

- Rebuild constant-frame-rate scheduling around compact frame runs so unchanged source frames still produce real encoded samples at every requested output tick without filling the queue with duplicate job objects.
- Isolate capture and NVENC on separate D3D11 devices connected by a keyed shared-texture bridge, keeping encoder conversion and resource waits away from the live capture context.
- Convert each unique captured frame to a canonical NV12 surface once, then reuse it across repeated CFR ticks and copy it into bounded per-output NVENC surfaces.
- Add an OBS-style buffered synchronous NVENC path that keeps three outputs queued before locking the oldest bitstream, allowing input preparation, hardware encoding, and output retrieval to overlap.
- Keep asynchronous and legacy NVENC initialization paths as compatibility fallbacks, with developer switches for comparing buffered sync and still-frame duplication behavior.

### Save Performance

- Measure capture pressure relative to the state at save start so an existing encoder workload does not make every save throttle as though the save caused it.
- Separate queued fresh frames from queued repeat ticks in save pacing and diagnostics.
- Derive a minimum adaptive write rate from measured storage service, preventing a fast SSD from collapsing to the conservative 16 MiB/s fallback after transient pressure.
- Recover write throughput promptly after capture pressure clears while retaining bounded 512 KB writes and low I/O priority.

### Diagnostics And Validation

- Report queued fresh and repeated encoder ticks independently and log the measured storage-rate floor used during a save.
- Add deterministic coverage for compact CFR runs and measured-service write-rate floors.
- Verified native packet tests, the production Electron build, sustained rolling capture, and a 120-second 2560x1440 save containing 7,230 video packets with zero missing frame slots or save-time drops.
- The reference 428 MB save completed in 806 ms, and FFmpeg reported the resulting MP4 as 60 FPS with a 1:59.98 presentation duration.

## [1.2.2] - 2026-08-02

### Performance

- Add storage-aware adaptive save pacing: SSD output remains high-throughput while bounded sustained writes prevent large dirty-cache bursts, and slower or unknown storage learns a safer rate dynamically.
- Apply low Windows I/O priority to SSD output, very-low priority to seek-heavy storage, and reserve full background thread mode for sustained critical capture pressure.
- Stream replay archive ranges directly into the mux buffer, removing an extra disk-read scratch copy while retaining the 512 KB maximum output request.
- Move final directory creation, file placement, and clip-index persistence off Electron's synchronous main-thread path.
- Insert newly saved clips into the library incrementally instead of rescanning the full library twice at save completion.

### Audio Reliability

- Respect PCM container width, valid-bit depth, and block alignment when converting WASAPI input, including microphones that expose 24 valid bits inside 32-bit containers.
- Support packed 24-bit, 32-bit integer, and floating-point PCM conversion without misaligned samples that can sound like hiss or static.
- Run RNNoise processing only for its supported 48 kHz input rate while preserving normal capture at other device sample rates.

### UI And Diagnostics

- Make status notices dismiss automatically and keep settings notices scoped to the Settings tab.
- Add source-read, output-write, adaptive-rate, I/O-priority, and finalization-stage timing details for diagnosing slow saves without blocking capture.

### Validation

- Added native coverage for adaptive write-rate behavior, capture-pressure hysteresis, 512 KB write bounds, and PCM container conversion edge cases.
- Verified the native engine, packet architecture tests, Electron type checks, and production renderer build.

## [1.2.1] - 2026-08-02

### Performance

- Pipeline more converted NVENC input surfaces ahead as quality presets increase, giving P3 and higher enough conversion lead time to sustain the requested frame rate under game GPU load.
- Use non-blocking bitstream locks after asynchronous NVENC completion events, with bounded retries for transient driver contention instead of stalling encoder submissions.

### Audio Reliability

- Keep configured separate-app audio supervisors alive when an application is closed, launched late, or restarted, and automatically rebind capture to the new process.
- Select the root covering the largest matching process tree so Chromium-style applications include their helper audio processes while retaining one logical app track.
- Close process-loopback activation events and monitor target process lifetime so sessions restart cleanly without leaking native handles.

### Validation

- Added native coverage for the deterministic process-tree root selection used when app audio binds or rebinds.
- Verified sustained 60.00 FPS capture at 2560x1440 using NVENC P3 in the previously failing game workload.

## [1.2.0] - 2026-08-02

### New Features

- Added a rolling disk replay archive for encoded H.264 video and AAC audio, substantially reducing RAM usage for long replay windows.
- Added exact clip-window presentation: saves retain the preceding keyframe for reliable decoding while presenting precisely the requested duration.
- Added replay archive health, queued data, disk usage, RAM fallback, segment, failure, and maximum-write diagnostics.

### Performance

- Streamed disk-backed packets directly into the MP4 muxer without rebuilding the complete video in memory.
- Kept save writes capped at 512 KB and added adaptive pressure pacing that backs off when the live encoder queue needs time to recover.
- Moved normal replay persistence onto low-priority background workers while retaining a short hot packet window in RAM.

### Reliability

- Retain replay packets in RAM automatically if the disk archive cannot write, then retry without blocking capture threads.
- Keep active archive segments alive for in-progress saves and safely clean up abandoned replay sessions on the next launch.
- Prevent closed development console pipes from crashing Electron with an `EPIPE` main-process error.
- Added native coverage for disk persistence, retention, RAM fallback, bounded writes, snapshot lifetime, disk-backed MP4 muxing, and exact presentation edits.
