# limgen: technical document

How limgen is built. Read `design.md` first for what it does and why. Model
choices and benchmarks live in `models.md`.

Status: design. No app code yet; milestone L0 done, L1 next. Section 12
lists the milestones.

## 1. Stack

- C++17, Qt 6 (Core, Gui, Qml, Quick, QuickControls2, DBus, Network,
  Concurrent), qmake6 and make. Material style, as Omarchy's own apps.
- No Python, no PyTorch at runtime.
- Engine: stable-diffusion.cpp's `sd-cli` (https://github.com/leejet/stable-diffusion.cpp),
  pinned to one release (`master-1106-532d2c1`, 15 November 2026), built by
  limgen and run through `QProcess`.
- Tests: Qt Test, run offscreen.
- Target: Omarchy on Arch Linux, Wayland, Hyprland. NVIDIA, AMD and Intel
  GPUs; CPU as a slow fallback.

## 2. Layout

```text
limgen.pro
bin/build               qmake6 + make into build/limgen
bin/test                builds and runs the tests offscreen
bin/install             makepkg -fsi from pkgbuild/
bin/build-engine        builds sd-cli at the pinned release into build/engine/ (section 10)
bin/fake-sd-cli         test stand-in for sd-cli (section 11)
src/main.cpp            app setup; `limgen open` starts the window
src/cli.{h,cpp}         every headless command (section 9)
src/backend.{h,cpp}     window state for QML: batch, selection, status
src/theme.{h,cpp}       accent and corner rounding, from the Omarchy template
src/translator.{h,cpp}  Omarchy language, from the Omarchy template
src/omarchy.{h,cpp}     theme name, colors.toml, background folders, bg set, monitors
src/palette.{h,cpp}     palette parsing, colour names, prompt words (pure)
src/colorpass.{h,cpp}   Lab colour transfer on QImage (pure)
src/prompt.{h,cpp}      builds the full prompt from subject, palette, style (pure)
src/catalogue.{h,cpp}   model catalogue from src/models.json
src/modelstore.{h,cpp}  downloads, checksums, installed models
src/hardware.{h,cpp}    GPU, backend, memory detection
src/engine.h            abstract Engine
src/sdcppengine.{h,cpp} Engine over sd-cli: arguments, progress, cancel, errors
src/library.{h,cpp}     stored drafts and finals with metadata
src/keeper.{h,cpp}      finish a draft and install it into the theme folder
src/Main.qml            the window
src/*.qml               a few components (DraftGrid, PaletteBar, ...)
src/models.json         the model catalogue, compiled in
src/skill.md            the agent skill installed by `limgen skill install`
src/i18n/<code>.json    translations
src/resources.qrc
tests/limgen_tests.{pro,cpp}
tests/bench-tools.sh    tests for bench/ and bin/build-engine (no GPU, no network)
tests/data/             captured sd-cli help text and progress output
bench/                  L0 benchmark: pinned model list, fetch and run scripts, prompts
pkgbuild/PKGBUILD  pkgbuild/limgen.desktop  pkgbuild/limgen.svg
README.md  LICENSE  .gitignore
```

`src/` stays flat. No CMake, no generated files or model weights committed.

## 3. Components

- **Omarchy** (`omarchy.cpp`). Reads `theme.name` and
  `theme/colors.toml` under `~/.local/state/omarchy/current/`, lists the
  theme's backgrounds (the current theme copy and
  `~/.config/omarchy/backgrounds/<slug>/`), runs `omarchy theme bg set`, and
  reads `hyprctl monitors -j`. Watches the files with `QFileSystemWatcher`,
  re-arming after every change because a theme switch replaces files and
  symlinks. Every path has an environment override for tests (section 11).
- **Palette** (pure). Parses the flat `key = "value"` lines of `colors.toml`
  (no full TOML parser needed; unknown keys are ignored, a missing file is an
  error the window shows). Converts each colour to a descriptive name: hue
  family from a small table, qualifiers from Lab lightness and chroma
  ("deep", "muted", "pale", "near-black"). Picks background, foreground,
  accent and the two named colours with the highest chroma. Output: palette
  words, mood words from `mode`, and the Lab statistics used by the colour
  pass.
- **Prompt** (pure). Subject + palette words + mood + style preset words ->
  positive prompt; style preset -> negative prompt and default settings. The
  user's edits to the full prompt win over the builder.
- **Catalogue.** `models.json` lists each supported model: id, display name,
  publisher, licence id, short licence summary and URL, native sizes, files
  per quantisation level (role, URL, size, SHA-256), minimum free video
  memory per level, recommended sampler, steps and guidance, and the
  `sd-cli` options that model needs (for example `--llm` for a text encoder
  that is a language model). Adding a model is a catalogue change plus a
  benchmark row in `models.md`.
- **ModelStore.** Downloads catalogue files with `QNetworkAccessManager` to
  a `.part` file, resumes with HTTP range requests, checks size and SHA-256,
  renames into place. Never downloads without a confirmed request. Reports
  disk space before starting. Gated models are not in the catalogue.
- **Hardware.** Detects the backend `sd-cli` was built with, the GPU name and
  video memory: `nvidia-smi --query-gpu=name,memory.total,memory.used
  --format=csv,noheader,nounits` on NVIDIA, `mem_info_vram_total` and
  `mem_info_vram_used` under `/sys/class/drm/card*/device/` on AMD, shared
  memory and a low tier on Intel. Chooses the quantisation level and memory
  options (section 6).
- **Engine / SdCppEngine.** See section 4.
- **Library.** Stores every draft and final with a JSON sidecar
  (section 8) and a copy of the key fields in the image's own metadata (a
  PNG text chunk or a JPEG comment).
- **Keeper.** Finishes a draft (section 5, steps 4 to 6), writes the result
  beside its target in the theme folder, renames it into place, and records
  the action for undo.
- **Backend.** The QObject QML talks to: current batch, selection, progress,
  status text, estimates. Holds no file logic of its own.

## 4. Engine

### Why stable-diffusion.cpp

- One C++ binary, called like `ffmpeg` in Omarchy's other apps. No Python
  environment or 5 GB PyTorch install for users to manage.
- CUDA, Vulkan and ROCm backends: one app for NVIDIA, AMD and Intel.
- Runs GGUF-quantised models, which is what makes FLUX-class and newer models
  fit in 8 GB.
- Supports the model families in `models.md` (SDXL, FLUX.1, FLUX.2 klein,
  Z-Image, Qwen-Image, Chroma), image to image, ESRGAN upscaling, VAE tiling
  and CPU offload.

Risks: the project warns that its command-line options change often, and
the AUR packages are unofficial and patchy. Both are handled by pinning one
release and building it ourselves (section 10). Alternatives considered:
Hugging Face diffusers in a Python worker (best model coverage on day one,
but a heavy runtime and NVIDIA-centred), and driving a ComfyUI server (large
install, a second app to manage). `Engine` is an interface so a second engine
can be added if L0 shows `sd-cli` falls short.

### Invocation

- Text to image: `sd-cli` with the model files from the catalogue, `-p`,
  `-n`, `-W`, `-H`, `--steps`, `--cfg-scale`, `--sampling-method`, `-s`
  (seed), `-b` (batch count, so a batch loads the model once), `-o`.
- Image to image: adds `-i <source>` (`--init-img`) and `--strength`.
- Memory options as chosen by Hardware: `--vae-tiling`, `--offload-to-cpu`,
  `--backend te=cpu` (the text encoder on the CPU; `--clip-on-cpu` is
  deprecated at the pinned release), `--flash-attn`. The pinned release also
  has `--auto-fit` (on by default), which places weights on the GPU, in RAM
  or on disk to fit the memory available, and `--max-vram`, a budget that can
  reserve free memory for other programs. On CUDA, L0 found auto-fit alone
  ran every model with 5.1 GB free (section 6).
- Live previews: `--preview tae --tae <file> --taesd-preview-only
  --preview-path <cache>/preview-%03d.png --preview-interval 2`.
  - `tae` decodes the latent with a tiny autoencoder, one file of about
    10 MB per VAE family (taef1, taef2, taesdxl), listed in the catalogue
    and downloaded with the model. L0 chose it on every second step: 16%
    slower than no preview. `proj`, a straight projection of the latent to
    colours, cost 1% but shifted colours (`models.md` section 6).
  - Without `--taesd-preview-only` the engine also decodes the final image
    with the tiny autoencoder.
  - With the `%03d` placeholder the engine writes a new numbered file per
    preview, counting on across the batch, so the number says which draft
    and step it belongs to. It writes in place, not by rename, so
    SdCppEngine reports a preview only once the next one exists or the
    draft has finished.
- Exact option names are built in one function, `SdCppEngine::arguments()`,
  checked by tests against the pinned version's `--help` text.

### Progress, cancel, errors

- Progress comes from `sd-cli`'s step lines on stdout (`n/total` and
  seconds per step, redrawn with carriage returns); parsing is a pure
  function with tests on captured output (`tests/data/`). The time per step
  feeds the estimates. The first step of a batch is several times slower
  than the rest (6.6 s against 1.1 s for Z-Image Turbo on the reference
  card) because the weights are copied to the GPU then, so estimates count
  it apart.
- With `-b 4` the engine samples all four latents before it decodes any of
  them, so the finished drafts arrive together at the end of the batch.
  Until then, the previews are what show each draft forming.
- Cancel: `terminate()`, then `kill()` after two seconds; partial files are
  removed.
- Exit codes and stderr are mapped to user-facing errors: out of memory,
  missing file, unsupported model, backend failure. The exit code alone is
  not enough: at the pinned release, an ESRGAN upscale that runs out of
  memory logs `[E] upscale failed`, saves the unscaled input under the
  output name and exits 0 (seen in L0 with `--upscale-tile-size 512` on an
  8 GB card). SdCppEngine treats any `[E]` line as a failure and checks
  that every output file exists with the requested size.
- An out-of-memory exit is retried once with more offload; a second failure
  is reported with the memory figures.

## 5. Generation pipeline

1. Snapshot the palette, theme slug, model and settings for the batch.
2. Drafts: text to image (or image to image for a variation) at the model's
   native 16:9 size, for example 1344x768 for SDXL-class models, batch of 4.
3. Store drafts in the library with metadata.
4. On keep, finish: upscale with Real-ESRGAN x4plus (`sd-cli -M upscale
   --upscale-model ... --upscale-tile-size 256`), then scale to cover the
   target size (largest monitor) with Lanczos and crop the centre. Chosen in
   L0 over image to image at the final size, which was slower, needed the
   whole card and changed details of the draft (`models.md` section 6).
   The ESRGAN file (67 MB, BSD 3-Clause) is a catalogue entry downloaded with
   the first model.
5. Optional colour pass (`colorpass.cpp`): Reinhard transfer in Lab space
   toward the theme's statistics, blended at the chosen strength.
6. Encode as PNG or JPEG (the user's setting; PNG by default, JPEG at
   quality 92) with `QImageWriter`, write beside the target, rename into
   place.

The drafts are never modified; every final can be remade from its draft and
metadata.

## 6. Video memory

- Hardware reads total and free video memory before every batch, because
  other programs may hold some.
- Each catalogue model lists the minimum free memory for each quantisation
  level. limgen picks the best level that fits, then adds options in this order
  until the estimate fits: VAE tiling, text encoder on CPU, offload to CPU.
- The choice and the reason are shown in the window ("Q4 with CPU offload:
  4.1 GB free").
- Below the smallest level, the model is shown as not runnable on this
  machine rather than hidden.
- L0 result: on CUDA the pinned engine's auto-fit ran every candidate at
  1344x768 with 5.1 GB free and no memory options from limgen, 14 to 38%
  slower than with 6.7 GB free. If Vulkan behaves the same, the option
  ladder above shrinks to `--max-vram` for a busy card, to leave room for
  other programs. The catalogue also starts with one level per model, Q4
  (`models.md` section 2), so there is no level to choose yet. L2 settles
  this.

## 7. Files and paths

| Path | Contents |
|---|---|
| `~/.config/limgen/config.json` | chosen model, style, colour pass strength, output format (`png` or `jpeg`), JPEG quality |
| `~/.local/share/limgen/models/<model-id>/` | downloaded model files |
| `~/.local/share/limgen/library/<yyyy-mm>/<id>.{png,jpg}` and `.json` | drafts (always PNG) and finals (the output format) with metadata |
| `~/.local/state/limgen/bench.json` | measured seconds per step per model, level and backend |
| `~/.cache/limgen/` | temporary files during a batch |
| `~/.config/omarchy/backgrounds/<slug>/limgen-*.{png,jpg}` | kept backgrounds (the only Omarchy path written) |
| `/usr/lib/limgen/sd-cli` | the pinned engine build (section 10) |

All honour `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_STATE_HOME` and
`XDG_CACHE_HOME`.

## 8. Metadata

One JSON sidecar per image:

```json
{
  "id": "2026-11-20-a3f9c1",
  "kind": "draft",
  "created": "2026-11-20T10:12:31Z",
  "theme": { "slug": "tokyo-night", "mode": "dark", "palette": { "accent": "#7aa2f7", "background": "#1a1b26" } },
  "subject": "oil rig at dusk",
  "prompt": "...", "negative": "...", "style": "cinematic",
  "model": { "id": "...", "level": "q4_k", "files_sha256": ["..."] },
  "engine": { "name": "sd-cli", "version": "...", "backend": "vulkan" },
  "seed": 123456789, "steps": 8, "cfg": 1.0, "sampler": "euler", "size": [1344, 768],
  "source": null,
  "finish": null
}
```

A seed remakes a draft exactly only with the same engine build and
backend: in L0, CUDA and Vulkan gave the same composition for a seed but
not the same pixels. The `engine` field records both.

`source` holds the source image id or path and strength for variations;
`finish` holds the upscale method, colour pass strength, final size and
format. The same key fields go in the image itself, in the `parameters`
format other image tools read (a PNG text chunk, or a comment in a JPEG), so
the prompt survives copying the file elsewhere. Drafts get this from the
engine: the pinned `sd-cli` writes a `parameters` chunk with the A1111-style
text and its own JSON record (`sdcpp.generation/v1`). Finals are encoded by
limgen, which writes the chunk itself. The sidecar stays the source of
truth for both.

## 9. CLI

The window is `limgen open` (and `limgen` with no command). Every other
command runs headless (`QT_QPA_PLATFORM=offscreen` set before
`QGuiApplication`), takes `--json`, writes errors to stderr, and exits with
the codes below. Hype's `src/cli.cpp` is the pattern.

| Command | Does |
|---|---|
| `limgen palette` | current theme palette, palette words, mood |
| `limgen doctor` | engine version, backend, GPU, memory, models, problems found |
| `limgen models [list]` | catalogue with installed state and fit on this machine |
| `limgen models pull <id> [--level q4_k] [--yes]` | download (asks unless `--yes`) |
| `limgen generate <subject> [--count 4] [--style ...] [--model ...] [--seed ...] [--from <image>] [--strength 0.5] [--prompt <full>]` | make drafts, print ids and paths |
| `limgen keep <id> [--set] [--theme <slug>] [--color-pass 0.3]` | finish and install into the theme folder |
| `limgen set <file-or-id>` | `omarchy theme bg set` on it (keeps it first if it is a draft) |
| `limgen library [--theme <slug>]` | list stored images |
| `limgen skill install` | install the bundled agent skill |

Exit codes: 0 success, 1 error, 2 usage, 3 not runnable here (no backend,
not enough memory, model missing), 130 interrupted.

## 10. Packaging

- `pkgbuild/PKGBUILD` builds the app from a release tarball, installs the
  binary, `limgen.desktop` (with translated `Name` and `Comment`) and
  `limgen.svg`.
- limgen builds its own engine (decided; `design.md` section 11). The same
  PKGBUILD builds a pinned stable-diffusion.cpp release as split packages
  that each provide `limgen-engine` and install `/usr/lib/limgen/sd-cli`:
  `limgen-engine-vulkan` (default; NVIDIA, AMD, Intel), `limgen-engine-cuda`
  (optional: 4% faster on NVIDIA in L0, but needs the 5.2 GB `cuda`
  package for cuBLAS), possibly `limgen-engine-rocm`. Pinning means limgen's argument building
  always matches the binary it calls. Each engine package installs
  stable-diffusion.cpp's MIT licence beside limgen's.
- The pin (tag and commit) lives in `bin/build-engine`, which builds the same
  release into `build/engine/<backend>/bin/sd-cli` for development. The
  PKGBUILD must use the same tag and commit; L7 makes it read them from one
  place.
- The CUDA package builds for a list of architectures (Turing and newer);
  development builds use `native`.
- `LIMGEN_SD_CLI` overrides the engine path, for development and tests.
- Later: publish through `omacom/omarchy-pkgs` as the Omarchy app
  conventions describe, once a tagged release exists.

## 11. Testing

- `bin/test` builds `tests/limgen_tests` with `../src/*.cpp` compiled in and
  runs it offscreen, then runs `tests/bench-tools.sh`, the tests of the L0
  benchmark scripts (fake engine and `nvidia-smi`, no GPU, no network).
- Pure units with plain tests: palette parsing and colour names, prompt
  building, Lab conversion and colour transfer, `sd-cli` argument building,
  progress parsing (on captured real output), file naming, metadata round
  trip.
- `bin/fake-sd-cli` stands in for the engine: it prints captured progress
  lines, writes solid-colour PNGs of the requested size, and can be told to
  fail with an out-of-memory message or hang (for cancel tests).
- Environment overrides point every Omarchy and XDG path at temp folders:
  `LIMGEN_OMARCHY_STATE` (instead of `~/.local/state/omarchy/current`),
  `LIMGEN_OMARCHY_BACKGROUNDS` (instead of `~/.config/omarchy/backgrounds`),
  `LIMGEN_OMARCHY_BIN` (a fake `omarchy` that logs its arguments),
  `LIMGEN_HYPRCTL`, `LIMGEN_SD_CLI` and the XDG variables. Tests never read
  or write the real theme folders.
- Window checks: the offscreen QML harness from the Omarchy app conventions,
  screenshots with the default accent, and one language switch.
- Real engine runs are manual (they need a GPU and downloaded models) and are
  recorded in `models.md`.

## 12. Milestones

| # | Deliverable |
|---|---|
| L0 | Spike, no app code: build `sd-cli` at a pinned tag with Vulkan and CUDA (`bin/build-engine`); run the candidates in `models.md` on fixed prompts at 1344x768; record time, peak video memory and quality; pick the default model per tier, the NVIDIA backend, the finish method and the preview method; capture the pinned `--help` text and real progress output into `tests/data/` for L2's tests. |
| L1 | Skeleton: `limgen.pro`, `bin/` scripts, Theme, Translator, Omarchy, Palette, Prompt; empty window with the palette bar; `limgen palette`. |
| L2 | Engine: Hardware, Catalogue (hand-placed model files), SdCppEngine, Library; `limgen doctor`, `limgen generate`. |
| L3 | ModelStore: downloads with resume, checksums, licence prompt; `limgen models`. |
| L4 | Window: batch grid, progress, estimates, cancel, shortcuts, `?` overlay. |
| L5 | Keep: finish, colour pass, install into the theme folder, set, undo; `limgen keep`, `limgen set`. |
| L6 | Variations from existing backgrounds and library images; library view. |
| L7 | Package and polish: PKGBUILD with engine split packages, desktop file, icon, translations, README with screenshot, `limgen skill install`. |
| L8 | First release `v0.1.0`. |

## 13. Risks

- **Engine churn.** `sd-cli` options change between releases. Pinned build,
  one argument builder, tests against `--help`.
- **Model licences.** Some strong models restrict commercial use of their
  outputs or require accepting terms. The catalogue shows the licence before
  download and the metadata records it; gated models are left out.
- **Video memory.** 8 GB cards are the common case and other programs take a
  share. Section 6, plus clear refusal below the smallest level.
- **Vulkan speed on NVIDIA.** Measured in L0: 4% slower than CUDA per
  image, and faster when memory is short. Vulkan is the default.
- **Quality of colour matching.** Palette words alone may drift; the
  reference image and colour pass layers exist for that, and L0 should check
  how far words alone get.
