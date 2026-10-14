# krkrsdl3 (Mikage fork)

[简体中文](README.md) · English

This is a fork of the [krkrsdl3](https://github.com/krkrsdl3/krkrsdl3) engine core, used as the KiriKiri runtime of [Mikage Next](https://github.com/itsCheney/Mikage-Next), a visual novel player for iOS. krkrsdl3 is a cross-platform reimplementation of the KiriKiri (KAG / TJS2) engine on SDL3.

This repository holds the engine source only and does not build on its own. Builds go through [krkrsdl3_build](https://github.com/itsCheney/krkrsdl3_build), which includes it as the `cpp/` submodule.

## Branches

| Branch | Purpose |
| --- | --- |
| `mikage` | Stable runtime, used by Mikage `main` |
| `mikage-beta` | Test runtime, used by Mikage `beta` |
| `main` | Tracks upstream krkrsdl3 |

Merges always go `mikage-beta` → `mikage`.

## What this fork changes

**Embedded runtime**
- The engine runs inside a host iOS app as a framework instead of as a standalone SDL program.
- Games can be started, quit and switched many times in one process: graphics, events, media and script caches are fully reset between sessions.
- `System.exit` and script errors end the current game, not the host app.
- Quitting keeps the game’s own exit confirmation and save callbacks.

**Metal rendering**
- A native Metal render backend presents the game.
- Ordinary Layer composition can run on the GPU, including the common blend modes, masks, blur, shrink copies, affine and perspective transforms, span composition and the built-in extrans transitions. Anything unsupported falls back to the software path.
- Only the changed regions of a layer are uploaded, GPU readbacks are skipped when a whole layer is overwritten, and readbacks are asynchronous.
- Unchanged frames are not presented again, which lowers idle CPU use and heat.

**Emote (E-mote / PSB)**
- A new Emote animation implementation, modeled on AetherKrkr’s timeline behavior. It is experimental and off by default; the original implementation stays the default.
- The shared resource cache is sized to the device and trimmed under memory pressure, PSB decoding reads less, and players can render straight into the layer texture.

**Media and other**
- The host can name movies to skip (such as translation-group credit videos); a skipped movie reports end of playback immediately.
- The audio decode pool waits for work instead of polling, and system variables are saved at most once a minute while idle.
- Frame timing, layer workload and CPU read attribution are exported for the host’s performance overlay and diagnostic logs.

## Layout

| Path | Contents |
| --- | --- |
| `core/archive/` | XP3 and other archive formats |
| `core/main/` | Engine core: window, event loop, threads, session lifecycle |
| `core/media/` | Font, image, movie and sound decoding |
| `core/render/` | Layer system and rendering; `backend/` holds the Metal, OpenGL, Vulkan and software backends |
| `core/script/` | TJS2 native bindings |
| `environ/` | Platform and CPU architecture specific code |
| `plugins/` | Plugins, including `emoteplayer/` and `extrans/` |
| `tjs2/` | The TJS2 language core |

## Dependencies

Provided by krkrsdl3_build through vcpkg: SDL3, FFmpeg, zlib, oniguruma, libpng, libwebp, libjpeg-turbo, libvorbis, opusfile, FreeType, plutosvg / plutovg.

## License

- The engine source keeps the KiriKiri / Kirikiri Z license and the krkrsdl3 supplementary clause; see [LICENSE](LICENSE). The clause says that when you distribute binaries of a port of a commercial game that runs on this software but was not developed with it, any modified parts must be released as open source.
- The new Emote animation implementation in `plugins/emoteplayer/` is GPL-3.0-or-later; see [AETHER-NOTICE.md](plugins/emoteplayer/AETHER-NOTICE.md) and [LICENSE-AETHER-GPL-3.0.txt](plugins/emoteplayer/LICENSE-AETHER-GPL-3.0.txt). Turning it off does not remove that code from the binary.
- Part of the span blend shaders in `core/render/backend/` derive from plutovg (MIT); see [MetalSpanComposite-LICENSE.txt](core/render/backend/MetalSpanComposite-LICENSE.txt).

Thanks to everyone who contributed to KiriKiri, Kirikiri Z and krkrsdl3.
