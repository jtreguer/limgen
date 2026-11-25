---
name: limgen
description: |
  Make images with limgen, which runs an image model on the local GPU: images of
  anything at a chosen size and palette, and desktop backgrounds for the current
  Omarchy theme. Use for ANY request to generate an image locally, or to
  generate, vary, keep, set or list Omarchy backgrounds made to match a theme.
---

# limgen

limgen makes images on this machine's GPU: images of anything, at the size
asked for (`limgen image`, then `limgen upscale`), and backgrounds that match
the current Omarchy theme, added to that theme's backgrounds folder
(`~/.config/omarchy/backgrounds/<theme>/`) when they are kept. Nothing leaves
the machine except a model download the person agreed to.

Every command below needs no display, takes `--json`, writes errors to stderr,
and exits 0 on success, 1 on an error, 2 on bad arguments, 3 when the machine
cannot run it (no usable GPU, not enough video memory, a model missing) and
130 when interrupted. `limgen help <command>` prints its options.

## Workflow

1. Check: `limgen doctor --json` says whether the engine, the GPU and the
   default model are ready, and what is missing. `limgen palette --json`
   shows the theme's colours and the words they become in the prompt.
2. Models: `limgen models --json` lists them. Downloading one is several
   gigabytes under a licence the person must accept: show them the model, its
   licence and its size, and run `limgen models pull <id> --yes` only after
   they agree.
3. Drafts: `limgen generate "oil rig at dusk" --json` makes four drafts (about
   a minute on an 8 GB card) and prints their ids and paths. Look at the PNGs
   before choosing. `--style` takes photo, cinematic, painting or minimal;
   `--count` 1 to 8; `--seed` repeats a result.
4. Variations: `limgen generate --from <id-or-file> --strength 0.5 --json`
   starts from an existing image, keeping its colours and layout; a higher
   strength (up to 0.9) moves further, towards the subject if one is given.
   Without a subject, a library image's own prompt is used again.
5. Keep: `limgen keep <draft-id> --json` upscales the draft to the largest
   monitor, applies a light colour pass toward the theme and adds it to the
   theme's backgrounds. `--set` also shows it now; `limgen set <id-or-file>`
   shows any image now (a draft is kept first).
6. Library: `limgen library --json` lists every draft and kept image, newest
   first; `--theme <slug>` narrows it to one theme.

## Images of anything

1. Previews: `limgen image "a red fox in fresh snow" --size 1080x1920 --json`
   makes four previews in the shape of the final size (1920x1080, 2560x1440,
   3840x2160, 1080x1920, 2048x2048 or any size up to four times the preview;
   the default is the largest monitor's) and prints their ids and paths. The
   prompt is used as written, with no style words. `--palette` takes a
   theme's name or up to five colours (`"#1a1b26,#ff9e64"`); `--count` 1 to
   9; `--model` and `--seed` as for generate. The previews of one prompt
   look alike; `--variety framing` gives each its own framing and
   `--variety light` its own light too, at about a third more time.
2. Final: look at the previews, then `limgen upscale <id> --output
   ~/Pictures/fox.png --json` upscales the chosen one to its size and writes
   it (a `.jpg` name writes JPEG). Without `--output` the final stays in the
   library and its path is printed. These images never go into a theme's
   folder.

`--engine vulkan` or `--engine cuda` runs another installed engine build;
`limgen doctor --json` lists them under `engines`.

## Working with the person

- Generating uses the GPU for a minute or more and keeping for half a minute;
  say so before starting several.
- Only `limgen keep` and `limgen set` change what the person sees: keep adds a
  file to the theme's backgrounds, set changes the current background. Ask
  before setting a background they did not ask for.
- When a command exits 3, read stderr: it says what is missing and what to do.
  Do not work around it, for example by picking another model unasked.
- The window (`limgen`, or `limgen open`; `limgen open generic` for images of
  anything) does the same with previews, from the keyboard; it needs the
  person's desktop, so do not wait on it.
