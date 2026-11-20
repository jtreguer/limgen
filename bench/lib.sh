# Shared by the bench scripts: where the models are, and which files and
# settings each benchmark model uses. Source it; it defines no commands.

models=${LIMGEN_MODELS:-${XDG_DATA_HOME:-$HOME/.local/share}/limgen/models}

# model_setup <model> <level> <encoder> sets, for one benchmark model:
#   files     sd-cli options naming its weight files
#   settings  sd-cli options for steps, guidance and sampler
#   tae       its tiny preview decoder
#   negative  its negative prompt (empty for models that ignore it)
#   level, encoder  rewritten for models that have neither (SDXL)
# Levels are q4 or q8. Returns 2 for an unknown model, 3 for a missing file.
model_setup() {
  local model=$1 dir qwen i
  level=$2 encoder=$3 negative=
  qwen=$models/qwen3-4b/Qwen3-4B-$([[ $encoder == q4 ]] && echo Q4_K_M || echo Q8_0).gguf
  case $model in
    z-image-turbo)
      dir=$models/z-image-turbo
      files=(--diffusion-model "$dir/z_image_turbo-$([[ $level == q4 ]] && echo Q4_K || echo Q8_0).gguf"
        --vae "$dir/ae.safetensors" --llm "$qwen")
      settings=(--steps 8 --cfg-scale 1.0)
      tae=$dir/taef1.safetensors
      ;;
    flux2-klein-4b)
      dir=$models/flux2-klein-4b
      files=(--diffusion-model "$dir/flux-2-klein-4b-$([[ $level == q4 ]] && echo Q4_0 || echo Q8_0).gguf"
        --vae "$dir/flux2-vae.safetensors" --llm "$qwen")
      settings=(--steps 4 --cfg-scale 1.0)
      tae=$dir/taef2.safetensors
      ;;
    sdxl-base-1.0)
      dir=$models/sdxl-base-1.0
      level=fp16 encoder=-
      files=(-m "$dir/sd_xl_base_1.0.safetensors" --vae "$dir/sdxl-vae-fp16-fix.safetensors")
      settings=(--steps 30 --cfg-scale 6.0 --sampling-method dpm++2m --scheduler karras)
      tae=$dir/taesdxl.safetensors
      # Only SDXL uses guidance, so only SDXL reads a negative prompt.
      negative="text, watermark, signature, logo, people, blurry, lowres, jpeg artifacts"
      ;;
    *) return 2 ;;
  esac
  for ((i = 1; i < ${#files[@]}; i += 2)); do
    if [[ ! -f ${files[i]} ]]; then
      echo "missing ${files[i]}; run bench/fetch" >&2
      return 3
    fi
  done
}

# peak_vram_start <file> samples total video memory use every 100 ms into a
# file until peak_vram_stop, which sets vram_peak to the highest value in MiB.
# Call both in the same shell (not inside $(...)) so the sampler can be reaped.
peak_vram_start() {
  nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits -lms 100 >"$1" &
  vram_sampler=$!
  vram_file=$1
}
peak_vram_stop() {
  kill "$vram_sampler" 2>/dev/null || true
  wait "$vram_sampler" 2>/dev/null || true
  vram_sampler=
  vram_peak=$(sort -n "$vram_file" | tail -1)
}
