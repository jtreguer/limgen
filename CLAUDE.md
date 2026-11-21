# CLAUDE.md

limgen (Local IMage GENeration) is an Omarchy desktop app that generates
backgrounds for the current Omarchy theme with an image model running on the
local GPU, and adds the ones the user keeps to that theme's background folder.
Qt 6 Quick front end in C++, stable-diffusion.cpp's `sd-cli` as the engine.
The repository is public: write everything for the community.

Read `docs/design.md` (what and why), `docs/technical.md` (how) and
`docs/models.md` (which models and why) before changing behaviour. They are
the spec. If code and docs disagree, ask which one is right rather than
silently picking one, and update the docs in the same change as the code.

## Status

Design done, MIT licence. Milestones L0 (the engine spike, results in
`docs/models.md` section 6), L1 (the skeleton and `limgen palette`), L2
(the engine, `limgen doctor` and `limgen generate`), L3 (model downloads,
`limgen models`), L4 (the window) and L5 (keeping, `limgen keep` and
`limgen set`) are done; L6 is next (`docs/technical.md` section 12):

| # | Deliverable |
|---|---|
| L0 | `sd-cli` built at a pinned tag (Vulkan, CUDA); candidate models benchmarked; defaults chosen |
| L1 | skeleton, Theme, Translator, Omarchy, Palette, Prompt; `limgen palette` |
| L2 | Hardware, Catalogue, SdCppEngine, Library; `limgen doctor`, `limgen generate` |
| L3 | ModelStore downloads; `limgen models` |
| L4 | the window: batch grid, progress, cancel, shortcuts |
| L5 | keep, finish, colour pass, set, undo; `limgen keep`, `limgen set` |
| L6 | variations, library view |
| L7 | PKGBUILD with engine split packages, desktop file, icon, translations, README |
| L8 | `v0.1.0` |

Do not start a milestone before the earlier ones are done and tested.

## Conventions

limgen follows the conventions of Omarchy's own apps. If the `omarchy-app`
skill is available, load it before writing code; otherwise read the
published apps: Omacut (https://github.com/omacom-io/omacut), Hype
(https://github.com/omacom/hype) for the headless CLI, Monologue and Signed.
In short:

- One window, one job, keyboard first (`?` lists shortcuts, `Q` quits and asks
  when work would be lost, Space is the main action).
- C++ owns logic (QObjects with `Q_PROPERTY` and `Q_INVOKABLE`), QML owns
  layout and keys. `QGuiApplication`, Material dark style, theme accent,
  `theme.radius` for every corner.
- Every visible string through `qsTr()`/`tr()`, translations in
  `src/i18n/<code>.json`, numbers and dates through the locale.
- Heavy work through command-line tools via `QProcess` (`sd-cli`,
  `hyprctl`, `omarchy`), never by linking their libraries.
- Headless commands for everything the window does, each with `--json`.
- Comments explain why, in full sentences. Commit messages are short and
  imperative.
- Docs and UI text in plain English: short direct sentences, no marketing
  words.

## Commands

These exist from L0 on:

```
bin/build-engine [vulkan|cuda|all]   # sd-cli at the pinned release into build/engine/<backend>/bin/
bench/fetch [folder...]              # L0 model files into ~/.local/share/limgen/models/ (about 32 GB)
bench/run ...                        # L0 benchmark runs into build/bench/
bench/finish ...                     # L0 finish comparison (ESRGAN, image to image)
bench/palette-distance <run>...      # mean ΔE between images and their palette
bench/sheet <run>... / bench/score   # blind rating page, then scores per run
tests/bench-tools.sh [name]          # tests for the scripts above; no GPU or network
```

Until `bin/test` exists (L1), `tests/bench-tools.sh` is the suite that must
pass before a commit; from L1 on, `bin/test` runs it too.

The engine pin (tag and commit) lives in `bin/build-engine`. All builds need
`cmake` and `ninja`; Vulkan builds also need `vulkan-headers`,
`spirv-headers` and `shaderc`; CUDA builds need `cuda`.

These exist from L1 on:

```
bin/build      # qmake6 + make into build/limgen
bin/test       # builds and runs tests offscreen
bin/install    # makepkg -fsi: installs the app so it shows in the launcher
build/limgen open
```

Set `LIMGEN_SD_CLI` to a local `sd-cli` build during development, or to
`bin/fake-sd-cli` to work without a GPU.

## Review and tests before every commit

1. Tests for the change: written or extended, shown failing against broken
   code and passing against the change (the `aldo-test-specialist` agent
   when available).
2. A review of the diff and its tests (the `vera` agent when available). Fix
   what it finds, or record why a finding was rejected.

`bin/test` must pass. Mention in the commit message that both steps ran.

## Rules that must hold

- **The only Omarchy path limgen writes is
  `~/.config/omarchy/backgrounds/<slug>/`**, and only new `limgen-*` files
  there. Never modify `colors.toml`, theme folders, Omarchy config, or
  backgrounds limgen did not create.
- **Tests and experiments never touch the real Omarchy folders.** Use the
  `LIMGEN_*` and XDG overrides (`docs/technical.md` section 11) with temp
  folders.
- **No network access except a model download the user confirmed.** No
  telemetry, no update checks, no prompts sent anywhere.
- **Never commit model weights, generated images, `build/` or personal
  data.** Fixtures, screenshots and examples use made-up content and the
  default accent; no real user names, home paths or themes from a personal
  setup.
- Files are written beside their target and renamed into place. Library
  drafts are never modified after they are written.
- `sd-cli` arguments are built in one place (`SdCppEngine::arguments()`)
  for the pinned engine version only. Changing the pinned version means
  updating that function, its tests and `docs/models.md`.
- Pure modules stay pure: `palette`, `prompt`, `colorpass` and the progress
  parser do no I/O.
- A model is added only with its licence recorded in the catalogue and a
  benchmark row in `docs/models.md`. Gated models stay out of the catalogue.

## CLI exit codes

0 success, 1 error, 2 usage, 3 not runnable here (no backend, not enough
memory, model missing), 130 interrupted.
