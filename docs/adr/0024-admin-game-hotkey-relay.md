# ADR 0024: Save hotkey in games that run as administrator

Date: 2026-10-29

Status: implemented in 1.8.1; off until the user allows it.

## Context

Some games start elevated (Marvel Rivals does, through its launcher). Windows'
user interface privilege isolation then keeps raw input and keyboard hooks of
unelevated processes from seeing keys while that game has focus: Clipture's save
hotkey does nothing, as does Discord's mute key. Players worked around it by
running Clipture as administrator.

Running the whole app elevated would undo ADR 0020 (no administrator prompts,
per-user install) and give the WebView and every engine path administrator
rights. `uiAccess` would let an unelevated process receive that input, but only
from a signed binary installed in a protected folder, which the per-user install
is not.

## Decision

A relay: the engine itself, copied to `%ProgramFiles%\Clipture Hotkey Relay` and
started elevated by a per-user scheduled task.

- **Consent once.** The engine's game detection reports when the foreground game
  runs above its integrity level (`ProcessElevation`; a full-screen window the
  engine may not even query counts too). The host shows one click-through
  in-game card per game and session, then a card in the app with Allow and Not
  now. Allow runs `clipture_engine.exe --install-hotkey-relay <user SID>` behind
  Windows' prompt. It refuses to act for another account (over-the-shoulder
  elevation), copies itself to the protected folder and registers
  `\Clipture\Hotkey relay <SID>`: interactive token, highest run level, the user
  may read and run it, only administrators may change it. The prompt never
  appears while a game has focus, because it takes over the screen.
- **No silent escalation.** The task points at the copy in Program Files, which
  unelevated code cannot replace. A task running a file from the per-user folder
  would let any program the user runs gain administrator rights without a
  prompt.
- **Shortcuts only.** The relay forwards nothing but presses of the save
  shortcut, and only while an elevated window has focus. It accepts a shortcut
  only with Ctrl, Alt or Win, or a key that types nothing (F1-F24, Insert,
  Pause...), and changes at most every 5 s, so it cannot be turned into a way of
  reading what is typed in an admin window (`AdminHotkeyRelayProtocol.hpp`).
- **Short-lived.** The engine starts the task while an admin game is open and the
  setting is on, serves a named pipe, accepts only the relay copy as its client
  (by process image path), and tells it to quit two minutes after the game
  closes or when the setting goes off. The relay connects at identification
  impersonation level and exits when the pipe closes.
- **Versioned.** The pipe protocol starts with `clipture-hotkey-relay/1`. An
  installed relay is an older engine after updates; when the version no longer
  matches, the engine reports `outdated` and Allow installs the current copy.
- **Off and removal.** Settings turn it off without a prompt (the relay stays set
  up, so on again is instant) or remove it behind the prompt. UI saves never
  change the setting; only the Allow, off and remove commands do, so a page
  loaded before Allow cannot switch it back off.

## Limits

- Standard (non-administrator) Windows accounts must enter an administrator's
  password at the prompt, and the relay then runs for that account; the setup
  refuses that case.
- Uninstalling Clipture as a user does not remove the Program Files copy or the
  task; Settings > Remove from this PC does.
- Not yet confirmed against a real elevated game end to end; the parts were
  tested separately (protocol unit tests, build, UI states).
