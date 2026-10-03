# AetherKrkr animation integration attribution

This experimental integration references AetherKiri/AetherKrkr, revision
`fa0f8af9614865aebcb839abd32bc181643d1e4a` (2026-10-03).

Source: <https://github.com/AetherKiri/AetherKrkr/tree/fa0f8af9614865aebcb839abd32bc181643d1e4a>

The upstream repository declares GPL-3.0-or-later. The complete GPL v3 text is
preserved in `LICENSE-AETHER-GPL-3.0.txt`. Upstream Emote source files identify
LiDon as their original author and contain subsequent AetherKiri contributions.

The new `emoteanimation.h/.cpp`, associated integration additions, and new
animation tests are provided under GPL-3.0-or-later. They adapt the public
timeline/control behavior into a standalone C++17 core, rather than importing
the Godot renderer, PSB class hierarchy, or private AetherInternal backend.
Existing upstream notices and the KRKRSDL3 license continue to apply to their
respective original code.

Behavioral references:

- RuntimeSupport: independent timeline/track state and easing-weight conversion.
- PlayerRender: queued animator advancement, difference contribution, loop seek,
  timeline scheduling and the authored one-frame transition offset.
- PlayerQuery / EmotePlayer: optional variable arguments and ordinary versus D3D
  time units.

Mikage changes include typed resource adapters, per-player variables/selectors
and blink state, deterministic boundary processing, bounded cycle fast-forward,
portable state snapshots, clone isolation, optional diagnostics and the existing
KRKRSDL3 render interface. These are implementation choices awaiting real-game
comparison; they do not claim official E-mote SDK equivalence.

The integration is compiled into this experimental runtime even when its
playback mode is disabled. Selecting legacy mode does not remove the new code
or its license conditions from a built binary. Before distributing a combined
runtime/App, verify license compatibility, required source availability and all
binary-distribution notices; this experimental branch does not establish that
the existing project license alone covers the combined distribution.
