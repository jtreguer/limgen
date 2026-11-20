# limgen: models

Which image models limgen runs, why, and how they perform. Read with
`design.md` and `technical.md`. Research done in November 2026; the field
moves fast, so check the leaderboard and licences again before each release.

Status: models and the default engine backend chosen in milestone L0
(section 6), measured on an NVIDIA card. AMD and Intel GPUs are untested.

## 1. Requirements

- **Runs on an 8 GB GPU with other programs open.** The reference card is an
  NVIDIA RTX 3070 Ti (8 GB, of which 5.5 to 6.5 GB is typically free). AMD
  and Intel GPUs through Vulkan must have at least one model that works.
- **Supported by stable-diffusion.cpp** at the pinned version (GGUF
  quantisation, image to image).
- **Licence allows commercial use of outputs**, and the weights download
  without an account. Users publish themes; they should not have to check
  revenue caps or accept terms to use a background.
- **Strong at photoreal and cinematic scenes, and follows the prompt,
  including colour words.** Text rendering hardly matters.

## 2. Shortlist

| Role | Model | Size (image model + text encoder) | Licence | Steps | Notes |
|---|---|---|---|---|---|
| Default | Z-Image Turbo (Tongyi-MAI) | 6B DiT + Qwen3-4B, FLUX VAE | Apache 2.0, ungated | 8, guidance off | Clear step up from SDXL; any aspect ratio between 512² and 2048² of area. Catalogue level Q4_K (3.9 GB) with the encoder at Q4_K_M (2.5 GB) |
| Fast | FLUX.2 klein 4B (Black Forest Labs) | 4B + Qwen3-4B, FLUX.2 VAE | Apache 2.0, ungated | 4, guidance off | Smallest current model, twice as fast as Z-Image Turbo and closest to the palette in L0. Catalogue level Q4_0 (2.5 GB), sharing Z-Image's encoder file |

Chosen in L0 (section 6). Z-Image Turbo is the default on every card that
runs it: in L0 it ran with 5.1 GB free, 26% slower than with 6.7 GB. FLUX.2
klein 4B is offered beside it as the fast choice. Each model starts with one
level in the catalogue, Q4, because Q8 was 15 to 40% slower with no visible
gain and a palette distance within one point. A first install of the default is
6.8 GB (model, encoder, VAE, preview decoder, ESRGAN); adding klein is
2.8 GB more. Cards with less than about 5 GB free were not tested; if they
fail, a smaller model comes back for them.

The pinned release (`master-1106-532d2c1`) lists Z-Image (since December
2025), FLUX.2 klein and SDXL as supported. All three ran with it in L0.

Z-Image Turbo and FLUX.2 klein 4B use the same text encoder, Qwen3-4B (the
ComfyUI packages of both ship the identical `qwen_3_4b.safetensors`). The
catalogue can list it once and share the file, which saves 2.5 to 4.3 GB for
a user who installs both. L0 runs both models with one GGUF copy of it.

## 3. Considered and left out

| Model | Why not (for now) |
|---|---|
| FLUX 3 Image Klein (Oct 2026) | Best quality that fits in 8 GB (INT8 builds for RTX 30 cards), but gated download and the FLUX Kommunity licence limits commercial outputs (revenue cap, output review). Supported by stable-diffusion.cpp at the pinned release (GGUF by leejet). Candidate for an opt-in "bring your own token" entry later. |
| Qwen-Image 2.1 | Strong (leaderboard second), reportedly runs on 8 GB at Q4, and supported at the pinned release, but research / non-commercial licence. |
| Qwen-Image (first version) | Apache 2.0 and supported by stable-diffusion.cpp, but 20B; too heavy for 8 GB at useful quality. |
| FLUX.2 klein 9B | Non-commercial licence. |
| FLUX.2 dev, HunyuanImage 2.1 and 3.0, HiDream-I1, MiniMax H3-Image | Too large for 8 GB; several also have licence restrictions (gated encoder, excluded regions, non-commercial). |
| ERNIE-Image Turbo (Baidu) | Apache 2.0 and lists 1376x768 as a native size, but 8B with a 24 GB official requirement. Supported by stable-diffusion.cpp at the pinned release (GGUF by unsloth, Ministral 3 3B text encoder, FLUX.2 VAE), whose docs say it runs in 4 GB. Ranked just below Z-Image Turbo; worth a later benchmark row. |
| Chroma1-HD | Apache 2.0, supported by stable-diffusion.cpp, Q4 about 5.6 GB, but uses the large T5-XXL encoder and has no published quality comparison. Possible L0 extra. |
| SANA 1.5 / Sana Sprint, Fibo 2.0 | Non-commercial licences. |
| Kandinsky 5 Image Lite | MIT, but needed about 12 GB at 1024² in one RTX 3070 report. |
| Stable Diffusion XL base 1.0 | Shortlisted as the fallback and benchmarked in L0: usable images, but the slowest (21.9 s per image), the furthest from the palette (ΔE 17.5), and not needed as a fallback once both newer models ran with 5.1 GB free. Licence CreativeML Open RAIL++-M. |
| FLUX.1 dev / schnell | Superseded by FLUX.2 klein at the same or smaller size. |
| SD 3.5 Medium, Lumina 2 | Trail the shortlist; not rechecked. |

## 4. Quality context

Artificial Analysis open-weights text-to-image leaderboard, November 2026
(Elo, higher is better): MiniMax H3-Image 1066, Qwen-Image 2.1 1048, FLUX 3
Image Klein 1042, Fibo 2.0 1038, FLUX.2 dev 1000, **Z-Image Turbo 937**,
FLUX.2 klein 9B 931, ERNIE-Image Turbo 915, Qwen-Image 909, **FLUX.2 klein
4B 840**, FLUX.1 dev 814, FLUX.1 schnell 743. SDXL is not ranked; it sits
well below these.

Among models that are permissively licensed, ungated and fit in 8 GB, Z-Image
Turbo is the highest ranked.

## 5. Notes that shape the engine

- **Large text encoders.** Z-Image and FLUX.2 klein use a 4B language model
  (Qwen3-4B) as their text encoder, about as large as the image model.
  stable-diffusion.cpp loads it with its own option and can keep it on the CPU
  (`--clip-on-cpu`) or quantise it to GGUF. Encoding the prompt first and
  freeing the encoder before denoising is what makes these models fit.
- **RTX 30 cards have no FP8 compute.** FP8 files only save disk there; GGUF
  integer quantisation (Q4_K to Q8_0) is the useful route.
- **Resolution.** Generate drafts near one megapixel at 16:9 (1344x768 or
  1536x864) and upscale at keep time. Generating directly at 2560x1440
  distorts compositions on these models; the usual fix is an upscale followed
  by a light second pass (about 0.3 strength). L0 found that second pass
  redraws details of the draft, so limgen's finish uses an ESRGAN upscale
  alone (section 6).
- **Turbo models ignore negative prompts** (guidance off). Style presets
  must work through positive wording alone for Z-Image and FLUX.2 klein.

## 6. L0 benchmark

Method:

- Engine: `sd-cli` at `master-1106-532d2c1` (commit `532d2c1`, 15 November
  2026), built by `bin/build-engine` with CUDA and with Vulkan.
- Files: listed in `bench/models.tsv` (about 32 GB), each pinned to the
  publisher's repository commit and SHA-256, fetched by `bench/fetch` into
  `~/.local/share/limgen/models/<folder>/`.
- Models: Z-Image Turbo at Q4_K and Q8_0; FLUX.2 klein 4B at Q4_0 and Q8_0
  (the only levels its GGUF publisher offers); SDXL base 1.0 from the fp16
  safetensors with the fp16-fix VAE. The Qwen3-4B encoder at Q4_K_M and
  Q8_0, shared by the first two. Not done: converting SDXL to Q8_0 (it was
  dropped first) and a Chroma1-HD Q4 extra.
- Prompts: 20 fixed theme prompts, five subjects (industrial, spaceship,
  landscape, city, abstract-minimal) under four palettes (dark violet, dark
  green, warm dark, light), built by limgen's prompt rules once L1 exists, by
  hand before that.
- Size 1344x768, fixed seeds, batch of four.
- Measured: seconds per image (model load counted separately), peak video
  memory, whether it fits with 1.5 GB held by another process
  (`bench/run --hold 1.5`, which runs `bench/hold-vram.cu`), palette
  distance (`bench/palette-distance`: the six dominant colours of the image,
  each one's CIE76 ΔE to the nearest palette colour, averaged by area), and
  a blind 1 to 5 rating for quality and for "belongs to this theme".
- Runner: `bench/run <model> --level q4|q8 --encoder q4|q8 --backend
  cuda|vulkan --preview none|proj|tae`, which writes images and logs to
  `build/bench/<run>/` and one row per prompt to `build/bench/runs.tsv`.
- Also measured: finish at 2560x1440 by image to image versus an ESRGAN
  upscaler (Real-ESRGAN x4plus, BSD 3-Clause; time, memory, look), and the
  two combined (`bench/finish`). Drafts at 1344x768 are 7:4, not 16:9, so
  the finish crops about 1.6% of the height; 1536x864 would be exact at
  30% more pixels.
- Blind rating: `bench/sheet` builds a local page with the first image of
  each run per prompt, shuffled and unlabelled; `bench/score` maps the saved
  scores back to runs. Not done in L0: five runs make 100 images, more than
  one person wanted to score. With two runs it is 40.
- Previews: seconds per image with `--preview none`, `proj` and `tae`
  (taef1 for Z-Image, taef2 for FLUX.2 klein, taesdxl for SDXL), and
  whether `proj` is clear enough to judge a draft early.
- Memory: the engine's own placement (`--auto-fit`, with `--max-vram`
  reserving memory) against limgen's option ladder (`technical.md`
  section 6), both with 1.5 GB held by another process.

### Results

Measured on 20 November 2026 on the reference card (RTX 3070 Ti 8 GB,
driver 615.78, CUDA 13.4), with the desktop holding about 1.4 GB, so 6.5 to
6.8 GB were free. 20 prompts, batches of four at 1344x768, seed 42.

| Model | Level | Encoder | Backend | s / image | of which sampling | Peak card memory | Palette ΔE |
|---|---|---|---|---|---|---|---|
| Z-Image Turbo | Q4_K | Q4_K_M | CUDA | 12.2 | 10.1 | 6.0 GiB | 14.5 |
| Z-Image Turbo | Q4_K | Q4_K_M | Vulkan | 12.7 | 11.2 | 7.1 GiB | 14.5 |
| Z-Image Turbo | Q8_0 | Q8_0 | CUDA | 14.1 | 11.7 | 7.5 GiB | 14.3 |
| FLUX.2 klein 4B | Q4_0 | Q4_K_M | CUDA | 4.6 | 3.0 | 7.3 GiB | 13.1 |
| FLUX.2 klein 4B | Q8_0 | Q8_0 | CUDA | 6.4 | 4.2 | 6.5 GiB | 12.4 |
| FLUX.2 klein 4B | Q4_0 | Q4_K_M | Vulkan | 4.8 | 3.4 | 6.5 GiB | 13.1 |
| SDXL base 1.0 | fp16 | (built in) | CUDA | 21.9 | 20.1 | 7.0 GiB | 17.5 |

"s / image" is the wall time of the whole `sd-cli` run divided by four, so
it includes start-up, model loading and prompt encoding. Peak card memory
is the whole card, desktop included, and mostly shows how much the
engine's auto-fit chose to keep on the GPU, not what a model needs: every
model also ran with less (next table). All 140 batches succeeded.

Vulkan against CUDA on the same NVIDIA card: 4% slower per image overall
(11 to 14% slower sampling, partly made up by faster loading and
decoding), the same palette distance, and the same compositions for the
same seed, though not the same pixels (PSNR 24 to 40 dB between the two),
because small floating-point differences add up over the steps. Under
memory pressure Vulkan did better: with 5.1 GB free, Z-Image Turbo took
13.8 s per image on Vulkan and 15.5 s on CUDA. The CUDA build links cuBLAS
(about 580 MB, which on Arch comes only with the 5.2 GB `cuda` package);
the Vulkan build needs only the Vulkan loader and the GPU driver. Vulkan is
the default engine for every GPU, NVIDIA included; the CUDA package stays
an option for users who already have CUDA installed.

Palette ΔE by palette (lower is closer): every model found dark violet the
hardest (16 to 21) and was closest on dark green and light (11 to 16).
FLUX.2 klein was closest overall, by about 2 points over Z-Image Turbo; SDXL
was furthest. The quantisation level made no difference to it.

Busy card, with `bench/hold-vram.cu` holding 1.5 GiB more, so 5,092 to
5,202 MiB (about 5.1 GB)
free (industrial subject, four palettes):

| Model | s / image | Slower by | Peak card memory |
|---|---|---|---|
| Z-Image Turbo Q4_K | 15.5 | 26% | 7.4 GiB |
| Z-Image Turbo Q8_0 | 16.4 | 14% | 7.3 GiB |
| FLUX.2 klein 4B Q8_0 | 7.6 | 15% | 7.4 GiB |
| SDXL base 1.0 | 29.7 | 38% | 7.4 GiB |
| Z-Image Turbo Q4_K, Vulkan | 13.8 | 9% | 7.6 GiB |
| FLUX.2 klein 4B Q4_0, Vulkan | 5.1 | 6% | 6.4 GiB |

Nothing failed on either backend. The engine's auto-fit kept weights in RAM
and staged them to the GPU as needed, without any memory option from
limgen, which replaces most of limgen's own option ladder (`technical.md`
section 6). The Vulkan rows were measured with the klein level chosen in
the end, Q4_0, and the CUDA rows with Q8_0.

Previews (industrial subject, four palettes):

| Model | Preview | s / image | Cost |
|---|---|---|---|
| Z-Image Turbo Q4_K | none | 12.3 | |
| Z-Image Turbo Q4_K | `proj` | 12.4 | 1% |
| Z-Image Turbo Q4_K | `tae` | 15.6 | 27%, 0.9 GiB more |
| Z-Image Turbo Q4_K | `tae`, every 2nd step | 14.3 | 16% |
| Z-Image Turbo Q4_K, Vulkan | `tae`, every 2nd step | 13.4 | 6% |
| FLUX.2 klein 4B Q8_0 | none | 6.6 | |
| FLUX.2 klein 4B Q8_0 | `proj` | 7.0 | 6% |
| FLUX.2 klein 4B Q8_0 | `tae` | 8.0 | 21% |

`proj` previews are 168x96 (one pixel per latent cell) and show the
composition from step 2 of 8, but are soft at tile size and shift colours
(a pale sky came out pink), which misleads exactly when judging theme fit.
`tae` previews are full size and close to the final image. Every second
step (`--preview-interval 2`) still gives four previews per Z-Image draft
and two per FLUX.2 klein draft.

Finish of one draft to 2560x1440 (four drafts per model):

| Method | Z-Image Turbo Q4_K | FLUX.2 klein 4B Q8_0 | Peak card memory |
|---|---|---|---|
| ESRGAN x4, then Lanczos down | 28 s (Vulkan: 28 to 29 s) | 28 s | 2.0 GiB |
| Lanczos up, then image to image at 0.3 | 50 s | 36 s | 7.5 GiB |
| Both | 77 s | 64 s | 7.5 GiB |

Compared at 100%, ESRGAN keeps the draft exactly and sharpens edges, with
a slightly crisp look on metal. Image to image at 0.3 came out softer than
plain Lanczos and changed details (lamps moved, structures redrawn), which
defeats keeping the draft the user chose. ESRGAN is the finish method:
faster, a quarter of the memory, faithful to the draft. ESRGAN tile size 256
saves 1 s over the default 128; 512 ran out of memory (see the silent
failure noted in `technical.md` section 4). `bench/finish --tile N` repeats
these; it records a failed step even when the engine exits 0.

Earlier experience (before limgen, with Hugging Face diffusers on the
reference card): SDXL base in fp16 made usable 1344x768 drafts in about 15 s
each with model CPU offload; industrial subjects at dusk (oil rigs,
refineries, beached ships) worked well, interiors came out flat and
overlit. A 2048x1152 detail pass needed sequential CPU offload once another
process held 1.6 GB, at about 90 s per image.

## 7. Sources

- Leaderboard: https://artificialanalysis.ai/image/leaderboard/text-to-image/open-weights
- Z-Image Turbo: https://huggingface.co/Tongyi-MAI/Z-Image-Turbo and https://github.com/Tongyi-MAI/Z-Image
- Z-Image at 8 GB and resolution advice: https://www.thundercompute.com/blog/z-image-turbo-comfyui
- FLUX.2 klein: https://bfl.ai/blog/flux2-klein-towards-interactive-visual-intelligence
- FLUX.2 klein against Z-Image Turbo (palette, speed): https://tech.minimalcorp.com/en/posts/flux2-klein-vs-z-image-turbo
- FLUX 3 Image Klein: https://huggingface.co/black-forest-labs/flux-3-image-klein and https://bfl.ai/blog/flux-3-image-klein
- ERNIE-Image Turbo: https://huggingface.co/baidu/ERNIE-Image-Turbo
- Qwen-Image 2.1: https://www.marktechpost.com/2026/09/21/alibaba-qwen-releases-qwen-image-2-1/ and https://unsloth.ai/docs/models/qwen-image-2.1
- SANA 1.5 licence: https://huggingface.co/Efficient-Large-Model/SANA1.5_1.6B_1024px
- HunyuanImage 2.1: https://huggingface.co/tencent/HunyuanImage-2.1
- Chroma1-HD GGUF: https://huggingface.co/silveroxides/Chroma1-HD-GGUF
- Kandinsky 5: https://huggingface.co/docs/diffusers/en/api/pipelines/kandinsky5_image
- GGUF quantisation in diffusers: https://huggingface.co/docs/diffusers/main/en/quantization/gguf
- stable-diffusion.cpp supported models: https://github.com/leejet/stable-diffusion.cpp/blob/master/README.md
- GGUF files used in L0: https://huggingface.co/leejet/Z-Image-Turbo-GGUF, https://huggingface.co/leejet/FLUX.2-klein-4B-GGUF, https://huggingface.co/unsloth/Qwen3-4B-GGUF
- VAEs and the shared encoder in ComfyUI packaging: https://huggingface.co/Comfy-Org/z_image_turbo and https://huggingface.co/Comfy-Org/flux2-klein-4B
- Tiny preview decoders: https://huggingface.co/madebyollin/taef1, https://huggingface.co/madebyollin/taef2, https://huggingface.co/madebyollin/taesdxl
- Real-ESRGAN: https://github.com/xinntao/Real-ESRGAN
