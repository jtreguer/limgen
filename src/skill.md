---
name: limgen
description: |
  Make desktop backgrounds for the current Omarchy theme with limgen, which runs
  an image model on the local GPU. Use for ANY request to generate, vary, keep,
  set or list Omarchy backgrounds or wallpapers made to match a theme.
---

# limgen

limgen makes backgrounds that match the current Omarchy theme, on this
machine's GPU, and adds the ones that are kept to that theme's backgrounds
folder (`~/.config/omarchy/backgrounds/<theme>/`). Nothing leaves the machine
except a model download the person agreed to.

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

## Working with the person

- Generating uses the GPU for a minute or more and keeping for half a minute;
  say so before starting several.
- Only `limgen keep` and `limgen set` change what the person sees: keep adds a
  file to the theme's backgrounds, set changes the current background. Ask
  before setting a background they did not ask for.
- When a command exits 3, read stderr: it says what is missing and what to do.
  Do not work around it, for example by picking another model unasked.
- The window (`limgen`, or `limgen open`) does the same with previews, from
  the keyboard; it needs the person's desktop, so do not wait on it.
