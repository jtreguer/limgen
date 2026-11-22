# limgen: design document

limgen (Local IMage GENeration) makes desktop backgrounds for the current
Omarchy theme with an image model that runs on the user's own machine. This
document says what it does and why. `technical.md` says how, and
`models.md` records which models it runs and why they were chosen.

Status: milestones L0 (engine spike), L1 (skeleton), L2 (engine), L3 (downloads), L4 (the window), L5 (keeping) and L6 (variations, library) done, L7 next.

## 1. Purpose

Many Omarchy themes ship with one or two backgrounds, and some ship with none
that suit the user. Finding more that match a theme's palette and mood means
hours on wallpaper sites, and online image generators cost money per image,
send prompts to a third party and need an account.

limgen does one job: it generates new backgrounds that match the current
theme, on the local GPU, and adds the ones the user keeps to that theme's
background folder, where Omarchy's background cycling picks them up.

## 2. Background

- An Omarchy theme lives in `/usr/share/omarchy/themes/<slug>/` (stock) or
  `~/.config/omarchy/themes/<slug>/` (user). The active theme is copied to
  `~/.local/state/omarchy/current/theme/`, its slug is in
  `~/.local/state/omarchy/current/theme.name`.
- The theme's colours are in `colors.toml`: `mode` (`dark` or `light`),
  `accent`, the `background` and `foreground` families and eight named
  colours with bright variants.
- Backgrounds come from the theme's `backgrounds/` folder plus
  `~/.config/omarchy/backgrounds/<slug>/`, the folder Omarchy reserves for
  the user's own additions (`omarchy theme bg install` opens it).
  `omarchy theme bg next` cycles through both, `omarchy theme bg set <file>`
  shows one now.
- Open-weight image models (SDXL, FLUX, Z-Image, Qwen-Image and others) run
  on consumer GPUs with 8 GB of memory or less once quantised, through
  stable-diffusion.cpp, a C++ runtime with CUDA, Vulkan and ROCm backends.

## 3. Goals

1. Generate 16:9 (or the monitor's own ratio) backgrounds at the resolution
   of the user's largest monitor, from a short subject prompt.
2. Match the theme without the user describing colours: the palette and the
   light or dark mode of the current theme shape every prompt, and an
   optional colour pass brings the result closer to the palette.
3. Make variations of an existing background (the theme's own or one already
   generated) that keep its palette and composition.
4. Keep, discard, set as background, all from the keyboard.
5. Run entirely offline after the first model download. No account, no
   telemetry, no prompts leaving the machine.
6. Work on NVIDIA, AMD and Intel GPUs, and say clearly when the hardware is
   too weak, instead of failing slowly.
7. Be scriptable: every action the window offers also exists as a command
   that needs no display, so scripts and coding agents can drive it.
8. Follow the Omarchy app conventions: one window, one job, theme accent and
   corner rounding, the user's language, installed as an Arch package.

## 4. Non-goals

- A general image studio. No inpainting canvas, no node graphs, no LoRA
  training, no video. ComfyUI and InvokeAI exist for that.
- Editing themes. limgen never changes `colors.toml`, theme folders or any
  Omarchy config. It only adds files to the user background folder.
- Hosting or redistributing model weights. They are downloaded from their
  publishers at the user's request, with the licence shown first.
- Cloud generation, accounts, prompt sharing.
- Portrait or social formats. Backgrounds only.
- Replacing Omarchy's background switcher or per-monitor tools.

## 5. Principles

- **One job.** Every feature must serve "more backgrounds for this theme".
- **Local and private.** Network access only to download a model the user
  chose, and nothing else.
- **Honest about cost.** Before a model download, show its size, licence and
  the disk space left. Before a generation, show the expected time on this
  hardware once it is known.
- **Never touch the user's originals.** Theme backgrounds are read, never
  written. New files are written beside their target and renamed into place.
- **Reproducible.** Every image records the prompt, seed, model, settings and
  theme it was made with, so it can be made again or varied.
- **Say what's wrong.** No GPU backend, not enough memory, a missing or
  corrupted model, a theme without `colors.toml`: each is reported in the
  window and on stderr, with what to do next.

## 6. Concepts

- **Theme palette.** The colours read from the current `colors.toml`, plus
  `mode`. limgen turns them into words a model understands ("deep indigo",
  "soft lavender", "near-black") and into target colours for the colour pass.
- **Subject.** What the user types: "oil rig at dusk", "derelict starship in
  a hangar". Short by design.
- **Prompt.** Subject + palette words + mood words + the style preset. The
  user sees the full prompt and can edit it.
- **Style preset.** A named bundle of prompt words and settings: Photo,
  Cinematic, Painting, Minimal. The default is Cinematic.
- **Batch.** One generation request: usually four variants with different
  seeds, made as low-resolution drafts.
- **Draft and final.** Drafts are made at the model's native size (about one
  megapixel at 16:9) to be fast. Keeping a draft finishes it: an ESRGAN
  upscale to the monitor's resolution, then the optional colour pass.
- **Variation.** A new batch that starts from an existing image (image to
  image) instead of noise. Strength decides how far it may move.
- **Keep.** Writes the final image into
  `~/.config/omarchy/backgrounds/<slug>/` with a readable name.
- **Library.** Every draft and final limgen made, with its metadata, in
  limgen's own data folder. Kept images are copies, so clearing the library
  never removes a theme background.
- **Model.** A set of files (diffusion model, text encoder, VAE) plus
  recommended settings, described in limgen's model catalogue. The user may
  install several and pick one per batch.
- **Engine.** The program that runs a model. The first engine is
  stable-diffusion.cpp's `sd-cli`, run as a child process.

## 7. User workflows

### 7.1 First run

1. The window opens on a short hardware check: GPU name, backend found
   (CUDA, Vulkan, ROCm or CPU only), video memory, free disk space.
2. It proposes the default model for that hardware (see `models.md`), with
   download size, licence summary and a link to the licence. Other models are
   listed below it.
3. The user confirms. The download shows progress, can be paused, resumes
   after a restart and is verified by checksum.
4. The first batch measures how fast this machine is; limgen keeps the
   timings and shows an estimate before every batch from then on. (A
   separate test generation was planned; the first real batch gives the
   same numbers without making the user wait for a throwaway image.)

If no usable backend exists, the window says so, explains what is missing
(driver, Vulkan loader, ROCm) and offers CPU mode only if a test shows it can
finish a draft in a few minutes.

### 7.2 Making backgrounds

1. The window shows the current theme's name, its palette as swatches and
   its existing backgrounds as small thumbnails.
2. The user types a subject and presses Space (or Enter in the field).
3. Each of the four tiles shows a live preview that sharpens as the model
   works on that draft, a progress bar and a time estimate. The finished
   drafts replace the previews at the end of the batch. Esc cancels.
4. The user moves through them with the arrow keys, opens one full screen
   with F, and keeps it with K. Keeping finishes the draft in the background
   and adds it to the theme folder. A new batch asked for meanwhile waits
   until the kept drafts are finished, since both need the GPU.
5. S sets the selected image as the background now (keeping it first).
   D discards a draft. Ctrl+Z undoes the last keep or discard, and
   Ctrl+Shift+Z redoes it, until the next batch starts.
6. V starts a variation batch from the selected image.
7. R reruns the same prompt with new seeds.

### 7.3 Variations of an existing background

The theme's existing backgrounds are listed in the window. Selecting one
(a click, or B and Shift+B to step through them) makes it the source:
the next batch keeps its colours and layout while changing details, or,
at a higher strength, follows the subject typed in the prompt field. V
does the same with the selected draft, or with an image in the library
(L). The source stays for the following batches until it is cleared
(Clear, or Esc once nothing else is open); a background also stops being
the source when the theme changes. With nothing typed, a library image's
variations reuse its prompt, which the full prompt (P) then shows. This is the quickest way to get more
backgrounds that clearly belong to a theme.

### 7.4 Theme switch while open

The palette, the theme name, the existing backgrounds and the keep target all
follow a theme switch live. A batch that is running finishes with the palette
it started with; its images are marked with the theme they were made for, and
keeping one asks which theme to add it to if the theme changed.

### 7.5 From scripts and agents

`limgen generate "oil rig at dusk" --count 4 --json` makes drafts, adds
them to the library and prints their ids and paths. `limgen keep <id>` finishes and
installs one. `limgen palette --json` prints the current palette and the
words it becomes. Section 9 of `technical.md` lists every command.

## 8. Interface

One window, dark Material style, the theme accent, corners from Hyprland's
rounding, as in Omarchy's own apps.

- **Top:** theme name, palette swatches, the model in use, the GPU in use.
- **Left column:** the theme's existing backgrounds (stock and user), small,
  scrollable. Selecting one makes it the source for a variation.
- **Centre:** the current batch as a 2x2 grid of drafts with progress and a
  live preview while each one is made, or a single image full size, or
  the library: everything made for the current theme, newest first.
- **Bottom:** the subject field, the style preset, the editable full prompt
  and how kept images are finished (colour pass strength, PNG or JPEG),
  folded together by default (P), the variation source with its strength
  (shown for variations), Generate.
- A status line for errors and estimates.

Shortcuts:

| Key | Action |
|---|---|
| Space | Generate (or Enter in the subject field) |
| Esc | Close full screen or the library, leave a field, cancel the running batch, or clear the variation source |
| Arrow keys | Move between drafts |
| F | Full screen view of the selected image |
| K | Keep: finish and add to the theme's backgrounds |
| S | Keep and set as the current background |
| V | Variation batch from the selected image |
| B / Shift+B | Make the theme's next or previous background the variation source |
| L | Show or hide the library |
| R | Same prompt, new seeds |
| D | Discard the selected draft (Ctrl+Z restores it) |
| Ctrl+Z / Ctrl+Shift+Z | Undo / redo the last discard or keep |
| M | Choose the model |
| P | Show or hide the full prompt |
| ? | Show every shortcut |
| Q | Quit (asks first while a batch, a keep or a download runs) |

## 9. Matching the theme

Image models follow words, not hex codes. limgen matches a theme in three
layers, each optional after the first:

1. **Palette words.** The background, foreground, accent and the two most
   saturated named colours are converted to descriptive colour names with a
   lightness and saturation qualifier ("deep indigo", "muted lavender",
   "near-black"). `mode = "light"` adds bright, airy, high-key words; `dark`
   adds night, low-key words. The words go into the prompt; the user sees and
   can edit them.
2. **Reference image.** A variation from an existing background carries its
   palette directly. This is the strongest match and the recommended start
   for themes that already have one good background.
3. **Colour pass.** After generation, a colour transfer in Lab space moves
   the image's colour statistics toward the theme's (from its existing
   backgrounds when there are any, from the palette otherwise, which only
   moves the colour cast). It has a strength slider, defaults to light
   (0.3), and never runs on the stored draft, only on the final copy.

## 10. Omarchy integration

- Reads `~/.local/state/omarchy/current/theme.name` and
  `~/.local/state/omarchy/current/theme/colors.toml`, and watches both.
- Reads, never writes, the theme's `backgrounds/` folder.
- Writes kept images only to `~/.config/omarchy/backgrounds/<slug>/`, named
  `limgen-<subject-slug>-<short-id>.<ext>` so they are recognisable and never
  collide.
- Sets a background with `omarchy theme bg set <file>`, nothing else.
- Reads monitor sizes from `hyprctl monitors -j` to choose the final
  resolution (the largest monitor; the ratio follows that monitor).
- Follows the accent, corner rounding and language as described in the
  Omarchy app conventions.
- When Omarchy is not there (another Hyprland setup), limgen still runs:
  default accent, no palette unless one is chosen by hand, keep writes to
  `~/Pictures/limgen/`.

## 11. Decisions

- Name `limgen`: binary, package, desktop file, icon and Wayland app id.
- Qt 6 Quick front end in C++, built with qmake, as Omarchy's own apps.
- First engine: stable-diffusion.cpp (`sd-cli`) as a child process. No Python
  in the runtime path. Reasons and alternatives in `technical.md` section 4.
- The Vulkan engine is the default on every GPU, NVIDIA included. In L0 it
  was 4% slower than CUDA on NVIDIA and needs no CUDA install (5.2 GB on
  Arch); the CUDA engine stays an optional package.
- Models are downloaded at the user's request from their publishers, never
  bundled. The default is Z-Image Turbo, with FLUX.2 klein 4B offered as
  the fast choice, both at Q4 and sharing one text encoder file (chosen in
  L0; `models.md` sections 2 and 6).
- Only `~/.config/omarchy/backgrounds/<slug>/` is ever written in Omarchy's
  folders.
- Licence: MIT, as Omarchy's apps. Code taken from Omacut or Monologue keeps
  their MIT attribution.
- limgen builds its own copy of stable-diffusion.cpp at the pinned release,
  one package per GPU backend (`technical.md` section 10). Users need no GPU
  toolchain and no separate engine package, and the engine always matches the
  arguments limgen builds.
- Drafts show live previews while they are made, using the engine's own
  preview output with a tiny decoder on every second step (`technical.md`
  section 4). In L0 this cost 16% against 1% for the cheaper latent
  projection, but the projection shifted colours, and colour is what the
  user judges a draft by.
- Keeping a draft finishes it with an ESRGAN upscaler, not a second pass of
  the image model: in L0 it was faster, used a quarter of the memory and
  kept the draft as the user saw it, where the second pass redrew details
  (`models.md` section 6).
- Kept images are PNG (lossless, 4 to 9 MB at 2560x1440) or JPEG (under
  1 MB), the user's choice in settings. PNG is the default. WebP is not
  written, since Qt needs an extra plugin for it and the two formats cover
  both needs.

## 12. Open questions

- AMD and Intel GPUs: Vulkan is expected to work there, as it does on
  NVIDIA, but L0 had only an NVIDIA card to measure.
- What runs on cards with less than about 5 GB free: not tested in L0.
- Whether limgen's memory option ladder is needed at all, now that the
  pinned engine fits models to free memory itself: settled on CUDA by L0,
  still to check on Vulkan and AMD.
