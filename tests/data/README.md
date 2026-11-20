# Test data

Output captured from the pinned engine, `sd-cli` `master-1106-532d2c1`
(CUDA build from `bin/build-engine`), for the argument and progress parser
tests in L2. Recapture both files when the pin changes.

- `sd-cli-help.txt`: `sd-cli --help`. Its second line, `Usage: ...`, holds
  the path the binary was run from, so tests skip it.
- `sd-cli-z-image-batch4.log`: one Z-Image Turbo Q4_K batch of four at
  1344x768 with `-v`, stdout and stderr merged as `bench/run` records them.
  Progress lines are redrawn with carriage returns and end in an ANSI
  erase-line sequence. The prompt is a benchmark prompt; model paths were
  replaced with `/models` and output paths with `/out`, here and below.
- `sd-cli-klein-vulkan-batch4.log`: the same for FLUX.2 klein 4B Q4_0 (four
  steps) on the Vulkan build.
- `sd-cli-esrgan-oom-exit0.log`: an ESRGAN upscale (`-M upscale`, tile size
  512) that ran out of video memory. It logs `[E] upscale failed`, then
  saves the unscaled input as if it had worked, and the process exits 0.
  SdCppEngine must report this as a failure.
