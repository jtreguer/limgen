#!/usr/bin/env bash
# Tests for the L0 benchmark tooling in bench/ and bin/build-engine.
#
# Needs only bash, python3 with numpy, ImageMagick, curl and sha256sum. It uses
# no GPU, no engine build, no network (downloads use file:// URLs) and no model
# weights. Everything it writes goes to one temporary folder, and HOME and the
# XDG folders point there too, so a script that ignored an override would still
# not reach the real model folder or the Omarchy folders.
#
# Prints one line per test and exits 1 if any test failed.
# Usage: tests/bench-tools.sh [name-filter]
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/limgen-bench-test.XXXXXX")
trap 'rm -rf "$T"' EXIT

export HOME=$T/home XDG_DATA_HOME=$T/home/.local/share XDG_CONFIG_HOME=$T/home/.config
mkdir -p "$HOME"
unset LIMGEN_MODELS LIMGEN_BENCH_OUT LIMGEN_BENCH_LIST LIMGEN_SD_CLI

passed=0 failed=0
filter=${1:-}

# ---- helpers -------------------------------------------------------------
# Each test runs in a subshell with -e, so the first failing command ends it.
# The helpers below print why and return 1 to do that.

fail() { echo "    $*" >&2; return 1; }

eq() { [[ $1 == "$2" ]] || fail "$3: got '$1', expected '$2'"; }
exists() { [[ -e $1 ]] || fail "missing: ${1#"$T"/}"; }
absent() { [[ ! -e $1 ]] || fail "should not exist: ${1#"$T"/}"; }
contains() { grep -qF -- "$2" <<<"$1" || fail "output lacks '$2'; output was: $1"; }
lacks() { ! grep -qF -- "$2" <<<"$1" || fail "output should not contain '$2'"; }

# status <varname> cmd...  runs cmd, keeping its combined output in $out and
# its exit code in $rc, without tripping set -e.
status() {
  rc=0
  out=$("$@" 2>&1) || rc=$?
}

# number_near <value> <expected> <tolerance> <what>
number_near() {
  python3 -c 'import sys; v, e, t = map(float, sys.argv[1:4]); sys.exit(0 if abs(v - e) <= t else 1)' \
    "$1" "$2" "$3" || fail "$4: got $1, expected $2 +/- $3"
}

run_test() {
  local name=$1
  [[ -z $filter || $name == *"$filter"* ]] || return 0
  # The subshell must not sit inside an "if" or "||": bash switches set -e
  # off there, and a failed check would no longer end the test.
  local log=$T/test.log rc
  (set -e; cd "$T"; "$name") >"$log" 2>&1
  rc=$?
  if [[ $rc -eq 0 ]]; then
    echo "ok   $name"
    passed=$((passed + 1))
  else
    echo "FAIL $name"
    sed 's/^/     /' "$log"
    failed=$((failed + 1))
  fi
}

# ---- fetch ---------------------------------------------------------------

# Makes a fresh sandbox for fetch tests: $F/src holds the "remote" files,
# $F/list.tsv the list, $F/models the destination.
fetch_sandbox() {
  F=$(mktemp -d "$T/fetch.XXXXXX")
  mkdir -p "$F/src" "$F/models"
  printf 'alpha contents\n' >"$F/src/a.bin"
  printf 'beta contents, a little longer\n' >"$F/src/b.bin"
  : >"$F/list.tsv"
  export LIMGEN_MODELS=$F/models LIMGEN_BENCH_LIST=$F/list.tsv
}

# list_row <folder> <name> <source file> [size] [sha]
list_row() {
  local size=${4:-$(stat -c %s "$3")} sha=${5:-$(sha256sum "$3" | cut -d' ' -f1)}
  printf '%s\t%s\t%s\t%s\tfile://%s\n' "$1" "$2" "$size" "$sha" "$3" >>"$F/list.tsv"
}

test_fetch_downloads_verifies_and_renames() {
  fetch_sandbox
  list_row one a.bin "$F/src/a.bin"
  status "$root/bench/fetch"
  eq "$rc" 0 "exit code"
  cmp "$F/src/a.bin" "$F/models/one/a.bin"
  absent "$F/models/one/a.bin.part"
}

test_fetch_skips_comments_and_blank_lines() {
  fetch_sandbox
  { printf '# a comment\n\n'; } >"$F/list.tsv"
  list_row one a.bin "$F/src/a.bin"
  status "$root/bench/fetch"
  eq "$rc" 0 "exit code"
  eq "$(ls "$F/models")" one "folders created"
}

test_fetch_sha_mismatch_removes_part_and_fails() {
  fetch_sandbox
  list_row one a.bin "$F/src/a.bin" "" "$(printf 'x%.0s' {1..64})"
  status "$root/bench/fetch"
  eq "$rc" 1 "exit code"
  contains "$out" "SHA-256"
  absent "$F/models/one/a.bin"
  absent "$F/models/one/a.bin.part"
}

test_fetch_size_mismatch_fails_and_leaves_no_target() {
  fetch_sandbox
  local real
  real=$(stat -c %s "$F/src/a.bin")
  list_row one a.bin "$F/src/a.bin" $((real + 1))
  status "$root/bench/fetch"
  eq "$rc" 1 "exit code"
  contains "$out" "bytes, expected $((real + 1))"
  absent "$F/models/one/a.bin"
}

test_fetch_stops_at_first_failure() {
  # A bad file must stop the run: later files in the list are not fetched, so
  # a failure cannot scroll past unnoticed in a 32 GB download.
  fetch_sandbox
  list_row one a.bin "$F/src/a.bin" "" "$(printf '0%.0s' {1..64})"
  list_row two b.bin "$F/src/b.bin"
  status "$root/bench/fetch"
  eq "$rc" 1 "exit code"
  absent "$F/models/two/b.bin"
}

test_fetch_skips_existing_target() {
  fetch_sandbox
  mkdir -p "$F/models/one"
  printf 'already here' >"$F/models/one/a.bin"
  # The URL points at a missing file: a download attempt would fail the run.
  printf 'one\ta.bin\t5\t%s\tfile://%s/nowhere\n' "$(printf '0%.0s' {1..64})" "$F" >"$F/list.tsv"
  status "$root/bench/fetch"
  eq "$rc" 0 "exit code"
  contains "$out" "have one/a.bin"
  eq "$(cat "$F/models/one/a.bin")" "already here" "existing file contents"
}

test_fetch_second_run_changes_nothing() {
  fetch_sandbox
  list_row one a.bin "$F/src/a.bin"
  status "$root/bench/fetch"
  eq "$rc" 0 "first run exit code"
  local before
  before=$(stat -c %Y.%y "$F/models/one/a.bin")
  rm "$F/src/a.bin" # a second run must not need the source at all
  status "$root/bench/fetch"
  eq "$rc" 0 "second run exit code"
  contains "$out" "have one/a.bin"
  eq "$(stat -c %Y.%y "$F/models/one/a.bin")" "$before" "mtime of target"
}

test_fetch_folder_filter() {
  fetch_sandbox
  list_row one a.bin "$F/src/a.bin"
  list_row two b.bin "$F/src/b.bin"
  status "$root/bench/fetch" two
  eq "$rc" 0 "exit code"
  exists "$F/models/two/b.bin"
  absent "$F/models/one"
}

test_fetch_unknown_folder_filter_fetches_nothing() {
  fetch_sandbox
  list_row one a.bin "$F/src/a.bin"
  status "$root/bench/fetch" nosuch
  eq "$rc" 0 "exit code"
  absent "$F/models/one"
}

test_fetch_complete_part_is_checked_without_download() {
  # An interrupted run can leave a .part that is already complete. The URL is
  # missing here, so any download attempt would fail; the file must be accepted
  # on its checksum alone.
  fetch_sandbox
  mkdir -p "$F/models/one"
  cp "$F/src/a.bin" "$F/models/one/a.bin.part"
  list_row one a.bin "$F/src/a.bin"
  sed -i "s#file://.*#file://$F/nowhere#" "$F/list.tsv"
  status "$root/bench/fetch"
  eq "$rc" 0 "exit code"
  cmp "$F/src/a.bin" "$F/models/one/a.bin"
  absent "$F/models/one/a.bin.part"
}

test_fetch_corrupt_complete_part_is_rejected() {
  fetch_sandbox
  mkdir -p "$F/models/one"
  # Same size as the real file, different bytes.
  head -c "$(stat -c %s "$F/src/a.bin")" /dev/zero >"$F/models/one/a.bin.part"
  list_row one a.bin "$F/src/a.bin"
  sed -i "s#file://.*#file://$F/nowhere#" "$F/list.tsv"
  status "$root/bench/fetch"
  eq "$rc" 1 "exit code"
  absent "$F/models/one/a.bin"
  absent "$F/models/one/a.bin.part"
}

test_fetch_resumes_a_short_part() {
  fetch_sandbox
  mkdir -p "$F/models/one"
  head -c 6 "$F/src/b.bin" >"$F/models/one/b.bin.part"
  list_row one b.bin "$F/src/b.bin"
  status "$root/bench/fetch"
  eq "$rc" 0 "exit code"
  cmp "$F/src/b.bin" "$F/models/one/b.bin"
  absent "$F/models/one/b.bin.part"
}

test_fetch_download_error_leaves_no_target() {
  fetch_sandbox
  list_row one a.bin "$F/src/a.bin"
  sed -i "s#file://.*#file://$F/nowhere#" "$F/list.tsv"
  status "$root/bench/fetch"
  [[ $rc -ne 0 ]] || fail "expected a non-zero exit for a failed download"
  absent "$F/models/one/a.bin"
}

test_fetch_real_list_is_well_formed() {
  # Guards the checked-in list: five tab-separated fields, a 64-digit SHA-256
  # and an https URL on every non-comment row. Reads only; downloads nothing.
  local bad
  bad=$(awk -F'\t' '/^#/ || /^[[:space:]]*$/ {next}
    NF != 5 || $3 !~ /^[0-9]+$/ || $4 !~ /^[0-9a-f]{64}$/ || $5 !~ /^https:\/\// {print FNR": "$0}' \
    "$root/bench/models.tsv")
  eq "$bad" "" "malformed rows"
}

# ---- sheet ---------------------------------------------------------------

# prompt_ids prints "<palette>-<subject>" for every palette and subject in the
# checked-in tables, the same names bench/sheet looks for.
prompt_ids() {
  local p s
  for p in $(grep -v '^#' "$root/bench/palettes.tsv" | cut -f1); do
    for s in $(grep -v '^#' "$root/bench/subjects.tsv" | cut -f1); do
      echo "$p-$s"
    done
  done
}

# make_run <folder> <colour>  fills a run folder with one solid image per prompt.
make_run() {
  mkdir -p "$1"
  local id
  for id in $(prompt_ids); do
    magick -size 64x36 "xc:$2" "$1/$id-0.png"
  done
}

sheet_sandbox() {
  S=$(mktemp -d "$T/sheet.XXXXXX")
  export LIMGEN_BENCH_OUT=$S/out
  make_run "$S/runs/zturbo-secretname-q4" "#ff0000"
  make_run "$S/runs/klein-hidden-q8" "#00ff00"
  make_run "$S/runs/sdxl-private-fp16" "#0000ff"
}

test_sheet_page_does_not_reveal_run_names() {
  sheet_sandbox
  status "$root/bench/sheet" "$S/runs/zturbo-secretname-q4" "$S/runs/klein-hidden-q8" "$S/runs/sdxl-private-fp16"
  eq "$rc" 0 "exit code"
  local page name
  page=$(cat "$LIMGEN_BENCH_OUT/sheets/index.html")
  for name in zturbo secretname klein hidden sdxl private "$S"; do
    lacks "$page" "$name"
  done
  # Image file names are visible in the page source and in the browser, so
  # they must not carry run names either.
  lacks "$(ls "$LIMGEN_BENCH_OUT/sheets/img")" secretname
  lacks "$(ls "$LIMGEN_BENCH_OUT/sheets/img")" klein
  lacks "$(ls "$LIMGEN_BENCH_OUT/sheets/img")" sdxl
}

test_sheet_key_maps_every_letter_to_the_right_run() {
  # Each run has a distinct solid colour, so the image shown under a letter
  # proves the key sends that letter to the run that made the image.
  sheet_sandbox
  status "$root/bench/sheet" "$S/runs/zturbo-secretname-q4" "$S/runs/klein-hidden-q8" "$S/runs/sdxl-private-fp16"
  eq "$rc" 0 "exit code"
  local key=$LIMGEN_BENCH_OUT/sheets/key.tsv n=0 prompt letter run px want
  [[ $(sed -n 1p "$key") =~ ^"# sheet "[0-9a-f]+$ ]] || fail "key line 1 is not a sheet id: $(sed -n 1p "$key")"
  eq "$(sed -n 2p "$key")" $'prompt\tletter\trun' "key header"
  while IFS=$'\t' read -r prompt letter run; do
    n=$((n + 1))
    case $run in
      zturbo-secretname-q4) want=red ;;
      klein-hidden-q8) want=lime ;;
      sdxl-private-fp16) want=blue ;;
      *) fail "unknown run '$run' in key" ;;
    esac
    exists "$LIMGEN_BENCH_OUT/sheets/img/$prompt-$letter.jpg"
    # Red, green and blue channels of one pixel, 0 to 255.
    px=$(magick "$LIMGEN_BENCH_OUT/sheets/img/$prompt-$letter.jpg" \
      -format '%[fx:int(255*p{5,5}.r)] %[fx:int(255*p{5,5}.g)] %[fx:int(255*p{5,5}.b)]' info:)
    case $want in
      red) [[ $px == "255 0 0" || $px =~ ^2[45][0-9]\ [0-9]\ [0-9]$ ]] ;;
      lime) [[ $px =~ ^[0-9]\ 2[45][0-9]\ [0-9]$ ]] ;;
      blue) [[ $px =~ ^[0-9]\ [0-9]\ 2[45][0-9]$ ]] ;;
    esac || fail "$prompt $letter ($run): expected $want, pixel was '$px'"
  done < <(tail -n +3 "$key")
  local count
  count=$(prompt_ids | wc -l)
  eq "$n" $((count * 3)) "key rows (prompts x runs)"
  # Within a prompt every run gets its own letter, starting at A.
  local id
  for id in $(prompt_ids); do
    eq "$(awk -F'\t' -v p="$id" '$1 == p {print $2}' "$key" | sort | tr -d '\n')" ABC "letters of $id"
  done
}

test_sheet_page_has_a_rating_input_per_key_row() {
  sheet_sandbox
  status "$root/bench/sheet" "$S/runs/zturbo-secretname-q4" "$S/runs/klein-hidden-q8"
  eq "$rc" 0 "exit code"
  local page prompt letter run
  page=$(cat "$LIMGEN_BENCH_OUT/sheets/index.html")
  while IFS=$'\t' read -r prompt letter run; do
    contains "$page" "name=\"$prompt|$letter|quality\" value=\"5\""
    contains "$page" "name=\"$prompt|$letter|fit\" value=\"1\""
  done < <(tail -n +3 "$LIMGEN_BENCH_OUT/sheets/key.tsv")
}

test_sheet_run_missing_a_prompt_is_left_out_of_that_prompt() {
  sheet_sandbox
  local first
  first=$(prompt_ids | sed -n 1p)
  rm "$S/runs/klein-hidden-q8/$first-0.png"
  status "$root/bench/sheet" "$S/runs/zturbo-secretname-q4" "$S/runs/klein-hidden-q8"
  eq "$rc" 0 "exit code"
  local key=$LIMGEN_BENCH_OUT/sheets/key.tsv
  eq "$(awk -F'\t' -v p="$first" '$1 == p' "$key" | wc -l)" 1 "rows for the prompt with one run missing"
  eq "$(awk -F'\t' -v p="$first" '$1 == p {print $3}' "$key")" zturbo-secretname-q4 "remaining run"
}

test_sheet_replaces_old_sheets_and_keeps_siblings() {
  # bench/sheet deletes the sheets folder, so it must stay inside it: a
  # neighbouring file such as runs.tsv holds results that cannot be remade.
  sheet_sandbox
  mkdir -p "$LIMGEN_BENCH_OUT/sheets"
  echo stale >"$LIMGEN_BENCH_OUT/sheets/old-page.html"
  echo precious >"$LIMGEN_BENCH_OUT/runs.tsv"
  status "$root/bench/sheet" "$S/runs/zturbo-secretname-q4" "$S/runs/klein-hidden-q8"
  eq "$rc" 0 "exit code"
  absent "$LIMGEN_BENCH_OUT/sheets/old-page.html"
  eq "$(cat "$LIMGEN_BENCH_OUT/runs.tsv")" precious "sibling file"
}

test_sheet_one_run_is_a_usage_error() {
  sheet_sandbox
  status "$root/bench/sheet" "$S/runs/zturbo-secretname-q4"
  eq "$rc" 2 "exit code"
  absent "$LIMGEN_BENCH_OUT/sheets"
}

test_sheet_no_arguments_is_a_usage_error() {
  sheet_sandbox
  status "$root/bench/sheet"
  eq "$rc" 2 "exit code"
}

# ---- score ---------------------------------------------------------------

score_sandbox() {
  S=$(mktemp -d "$T/score.XXXXXX")
  printf '# sheet 5ca1ab1e\nprompt\tletter\trun\np1\tA\trunX\np1\tB\trunY\np2\tA\trunY\np2\tB\trunX\n' >"$S/key.tsv"
  # runX is p1/A and p2/B; runY is p1/B and p2/A. The fit scores are chosen so
  # that the two runs rank differently on quality and on fit, which a mixed-up
  # join or a swapped column would show.
  {
    printf '# sheet 5ca1ab1e\nprompt\tletter\tkind\tscore\n'
    printf 'p1\tA\tquality\t5\np1\tA\tfit\t4\n'
    printf 'p1\tB\tquality\t1\np1\tB\tfit\t2\n'
    printf 'p2\tA\tquality\t3\np2\tA\tfit\t5\n'
    printf 'p2\tB\tquality\t4\np2\tB\tfit\t1\n'
  } >"$S/ratings.tsv"
}

# score_field <run> <column>  prints one column of that run's line in $out.
score_field() { awk -v r="$1" -v c="$2" '$1 == r {print $c}' <<<"$out"; }

test_score_means_per_run() {
  score_sandbox
  status "$root/bench/score" "$S/ratings.tsv" "$S/key.tsv"
  eq "$rc" 0 "exit code"
  eq "$(score_field runX 2)" 4.50 "runX quality"
  eq "$(score_field runX 3)" 2.50 "runX fit"
  eq "$(score_field runX 4)" 2 "runX n"
  eq "$(score_field runY 2)" 2.00 "runY quality"
  eq "$(score_field runY 3)" 3.50 "runY fit"
  eq "$(score_field runY 4)" 2 "runY n"
}

test_score_ignores_ratings_without_a_key_row() {
  score_sandbox
  printf 'zz\tA\tquality\t5\nzz\tA\tfit\t5\np1\tC\tquality\t5\np1\tC\tfit\t5\n' >>"$S/ratings.tsv"
  status "$root/bench/score" "$S/ratings.tsv" "$S/key.tsv"
  eq "$rc" 0 "exit code"
  eq "$(score_field runX 2)" 4.50 "runX quality"
  eq "$(score_field runX 4)" 2 "runX n"
  eq "$(grep -c '^run[XY] ' <<<"$out")" 2 "runs listed"
}

test_score_run_without_ratings_is_not_listed() {
  score_sandbox
  printf 'p3\tA\trunZ\n' >>"$S/key.tsv"
  status "$root/bench/score" "$S/ratings.tsv" "$S/key.tsv"
  eq "$rc" 0 "exit code"
  lacks "$out" runZ
}

# A sheet saved halfway can have quality scores for a run and no fit scores
# yet. The missing mean must show as "-" instead of crashing the whole report.
test_score_half_rated_run_shows_missing_mean() {
  score_sandbox
  printf 'p3\tA\trunZ\n' >>"$S/key.tsv"
  printf 'p3\tA\tquality\t4\n' >>"$S/ratings.tsv"
  status "$root/bench/score" "$S/ratings.tsv" "$S/key.tsv"
  eq "$rc" 0 "exit code"
  eq "$(score_field runZ 2)" 4.00 "runZ quality"
  eq "$(score_field runZ 3)" - "runZ fit"
  eq "$(score_field runZ 4)" 1 "runZ n"
  eq "$(score_field runX 2)" 4.50 "runX quality"
}

test_score_default_key_follows_bench_out() {
  score_sandbox
  mkdir -p "$S/out/sheets"
  cp "$S/key.tsv" "$S/out/sheets/key.tsv"
  LIMGEN_BENCH_OUT=$S/out status "$root/bench/score" "$S/ratings.tsv"
  eq "$rc" 0 "exit code"
  eq "$(score_field runY 3)" 3.50 "runY fit"
}

test_score_without_arguments_is_a_usage_error() {
  status "$root/bench/score"
  eq "$rc" 2 "exit code"
}

# ---- palette-distance ----------------------------------------------------

# The tests use the first palette of the checked-in table, so they follow it
# if its colours are edited.
pd_sandbox() {
  P=$(mktemp -d "$T/pd.XXXXXX")
  pname=$(grep -v '^#' "$root/bench/palettes.tsv" | sed -n 1p | cut -f1)
  pcolours=($(grep -v '^#' "$root/bench/palettes.tsv" | sed -n 1p | cut -f3-7))
  mkdir -p "$P/run"
}

# pd_value <image name>  prints the distance on that image's line in $out.
pd_value() { awk -F'\t' -v n="$1" '$2 == n {print $3}' <<<"$out"; }

test_palette_distance_exact_palette_is_near_zero() {
  pd_sandbox
  # Five equal stripes, one per palette colour.
  local args=() c
  for c in "${pcolours[@]}"; do args+=("(" -size 40x108 "xc:$c" ")"); done
  magick "${args[@]}" +append "$P/run/$pname-city-0.png"
  status python3 -I "$root/bench/palette-distance" "$P/run"
  eq "$rc" 0 "exit code"
  # Stripe edges blend when the image is resized to 192 pixels wide, which
  # costs a few units; the far-colour test below scores above 40.
  number_near "$(pd_value "$pname-city-0.png")" 0 5 "distance of the palette itself"
}

test_palette_distance_far_colour_is_large() {
  pd_sandbox
  magick -size 192x108 xc:'#ffff00' "$P/run/$pname-city-0.png"
  status python3 -I "$root/bench/palette-distance" "$P/run"
  eq "$rc" 0 "exit code"
  local d
  d=$(pd_value "$pname-city-0.png")
  python3 -c 'import sys; sys.exit(0 if float(sys.argv[1]) > 40 else 1)' "$d" || fail "yellow against a dark violet palette scored only $d"
}

test_palette_distance_is_weighted_by_coverage() {
  # A quarter of the image in a far colour must score about a quarter of the
  # all-far image. An unweighted mean of the dominant colours would give half.
  pd_sandbox
  magick -size 192x108 xc:'#ffff00' "$P/run/$pname-city-0.png"
  magick \( -size 144x108 "xc:${pcolours[0]}" \) \( -size 48x108 xc:'#ffff00' \) +append "$P/run/$pname-abstract-0.png"
  status python3 -I "$root/bench/palette-distance" "$P/run"
  eq "$rc" 0 "exit code"
  local full part
  full=$(pd_value "$pname-city-0.png")
  part=$(pd_value "$pname-abstract-0.png")
  number_near "$part" "$(python3 -c "print($full * 0.25)")" "$(python3 -c "print($full * 0.03)")" "quarter coverage"
}

test_palette_distance_unknown_prefix_is_ignored() {
  pd_sandbox
  magick -size 64x36 xc:red "$P/run/nonesuch-city-0.png"
  magick -size 64x36 "xc:${pcolours[0]}" "$P/run/$pname-city-0.png"
  status python3 -I "$root/bench/palette-distance" "$P/run"
  eq "$rc" 0 "exit code"
  lacks "$out" nonesuch
  contains "$out" "$pname-city-0.png"
  contains "$out" "mean of 1"
}

test_palette_distance_folder_with_no_known_images_prints_nothing() {
  pd_sandbox
  magick -size 64x36 xc:red "$P/run/nonesuch-city-0.png"
  status python3 -I "$root/bench/palette-distance" "$P/run"
  eq "$rc" 0 "exit code"
  eq "$out" "" "output"
}

test_palette_distance_without_arguments_is_a_usage_error() {
  status python3 -I "$root/bench/palette-distance"
  eq "$rc" 2 "exit code"
}

# ---- lib.sh model_setup --------------------------------------------------

# lib_run <script text> runs it in a fresh bash that has sourced lib.sh with
# LIMGEN_MODELS pointing at $M.
lib_run() {
  rc=0
  out=$(LIMGEN_MODELS=$M bash -c 'source "$1"; shift; eval "$1"' _ "$root/bench/lib.sh" "$1" 2>&1) || rc=$?
}

model_sandbox() { M=$(mktemp -d "$T/models.XXXXXX"); }
touch_files() { local f; for f in "$@"; do mkdir -p "$(dirname "$M/$f")"; : >"$M/$f"; done; }

zimage_q4_files=(z-image-turbo/z_image_turbo-Q4_K.gguf z-image-turbo/ae.safetensors qwen3-4b/Qwen3-4B-Q4_K_M.gguf)

test_model_setup_unknown_model_returns_2() {
  model_sandbox
  lib_run 'model_setup nosuch-model q4 q4; echo "rc=$?"'
  contains "$out" "rc=2"
}

test_model_setup_missing_file_returns_3_and_names_it() {
  model_sandbox
  lib_run 'model_setup z-image-turbo q4 q4; echo "rc=$?"'
  contains "$out" "rc=3"
  contains "$out" "z_image_turbo-Q4_K.gguf"
  contains "$out" "bench/fetch"
}

test_model_setup_all_files_present_returns_0() {
  model_sandbox
  touch_files "${zimage_q4_files[@]}"
  lib_run 'model_setup z-image-turbo q4 q4; echo "rc=$?"; echo "files=${files[*]}"'
  contains "$out" "rc=0"
  contains "$out" "--diffusion-model $M/z-image-turbo/z_image_turbo-Q4_K.gguf"
  contains "$out" "--llm $M/qwen3-4b/Qwen3-4B-Q4_K_M.gguf"
}

test_model_setup_level_picks_the_q8_file() {
  # Only the q4 files exist, so asking for q8 must fail on the q8 diffusion file.
  model_sandbox
  touch_files "${zimage_q4_files[@]}"
  lib_run 'model_setup z-image-turbo q8 q4; echo "rc=$?"'
  contains "$out" "rc=3"
  contains "$out" "z_image_turbo-Q8_0.gguf"
}

test_model_setup_encoder_is_chosen_separately_from_level() {
  model_sandbox
  touch_files "${zimage_q4_files[@]}"
  lib_run 'model_setup z-image-turbo q4 q8; echo "rc=$?"'
  contains "$out" "rc=3"
  contains "$out" "Qwen3-4B-Q8_0.gguf"
}

test_model_setup_flux2_needs_its_own_vae() {
  model_sandbox
  touch_files flux2-klein-4b/flux-2-klein-4b-Q4_0.gguf qwen3-4b/Qwen3-4B-Q4_K_M.gguf
  lib_run 'model_setup flux2-klein-4b q4 q4; echo "rc=$?"'
  contains "$out" "rc=3"
  contains "$out" "flux2-vae.safetensors"
}

test_model_setup_sdxl_ignores_level_and_encoder() {
  # SDXL has one fp16 build and no text encoder file, so no Qwen file is needed
  # and level and encoder are rewritten for the caller's log.
  model_sandbox
  touch_files sdxl-base-1.0/sd_xl_base_1.0.safetensors sdxl-base-1.0/sdxl-vae-fp16-fix.safetensors
  lib_run 'model_setup sdxl-base-1.0 q8 q8; echo "rc=$?"; echo "level=$level encoder=$encoder"; echo "negative=$negative"'
  contains "$out" "rc=0"
  contains "$out" "level=fp16 encoder=-"
  contains "$out" "negative=text, watermark"
}

test_model_setup_only_sdxl_has_a_negative_prompt() {
  model_sandbox
  touch_files "${zimage_q4_files[@]}"
  lib_run 'model_setup z-image-turbo q4 q4; echo "negative=[$negative]"'
  contains "$out" "negative=[]"
}

# ---- usage errors --------------------------------------------------------

test_run_without_arguments_is_a_usage_error() {
  status "$root/bench/run"
  eq "$rc" 2 "exit code"
  contains "$out" "Usage: bench/run"
}

test_run_bad_level_is_a_usage_error() {
  status "$root/bench/run" z-image-turbo --level q9
  eq "$rc" 2 "exit code"
}

test_run_bad_encoder_is_a_usage_error() {
  status "$root/bench/run" z-image-turbo --encoder fp16
  eq "$rc" 2 "exit code"
}

test_run_bad_preview_is_a_usage_error() {
  status "$root/bench/run" z-image-turbo --preview full
  eq "$rc" 2 "exit code"
}

test_run_unknown_option_is_a_usage_error() {
  status "$root/bench/run" z-image-turbo --frobnicate
  eq "$rc" 2 "exit code"
}

test_run_missing_engine_exits_3_and_writes_nothing() {
  export LIMGEN_BENCH_OUT=$T/run-out
  status "$root/bench/run" z-image-turbo --backend nosuch-backend
  eq "$rc" 3 "exit code"
  contains "$out" "no engine at"
  contains "$out" "nosuch-backend"
  absent "$LIMGEN_BENCH_OUT"
}

test_finish_without_arguments_is_a_usage_error() {
  status "$root/bench/finish"
  eq "$rc" 2 "exit code"
}

test_finish_without_model_is_a_usage_error() {
  status "$root/bench/finish" draft.png
  eq "$rc" 2 "exit code"
}

test_finish_without_drafts_is_a_usage_error() {
  status "$root/bench/finish" --model z-image-turbo
  eq "$rc" 2 "exit code"
}

test_finish_bad_size_is_a_usage_error() {
  status "$root/bench/finish" --model z-image-turbo --size big draft.png
  eq "$rc" 2 "exit code"
}

test_finish_unknown_option_is_a_usage_error() {
  status "$root/bench/finish" --model z-image-turbo --frobnicate draft.png
  eq "$rc" 2 "exit code"
}

test_finish_missing_engine_exits_3_and_writes_nothing() {
  export LIMGEN_BENCH_OUT=$T/finish-out
  status "$root/bench/finish" --model z-image-turbo --backend nosuch-backend draft.png
  eq "$rc" 3 "exit code"
  contains "$out" "no engine at"
  contains "$out" "nosuch-backend"
  absent "$LIMGEN_BENCH_OUT"
}

test_engine_pin_is_the_same_everywhere() {
  # Written once in the PKGBUILD; the C++ probe and the catalogue must name
  # the same release, or limgen would refuse the engine it was packaged with.
  local tag
  tag=$(sed -n 's/^_sdcpp_tag=//p' "$root/pkgbuild/PKGBUILD")
  [[ -n $tag ]] || fail "no _sdcpp_tag in pkgbuild/PKGBUILD"
  contains "$(cat "$root/src/sdcppengine.h")" "pinnedVersion = \"$tag\""
  contains "$(cat "$root/src/models.json")" "\"engine\": \"$tag\""
}

test_build_engine_unknown_backend_is_a_usage_error() {
  # Must fail before it clones or builds anything.
  status "$root/bin/build-engine" nosuch-backend
  eq "$rc" 2 "exit code"
  contains "$out" "usage: bin/build-engine"
}

# ---- fetch: oversize .part ----------------------------------------------

test_fetch_oversize_part_is_replaced() {
  # A .part larger than the file cannot be resumed into a correct file, so it
  # is dropped and fetched again.
  fetch_sandbox
  mkdir -p "$F/models/one"
  head -c "$(($(stat -c %s "$F/src/a.bin") + 10))" /dev/zero >"$F/models/one/a.bin.part"
  list_row one a.bin "$F/src/a.bin"
  status "$root/bench/fetch"
  eq "$rc" 0 "exit code"
  contains "$out" "larger than expected"
  cmp "$F/src/a.bin" "$F/models/one/a.bin"
  absent "$F/models/one/a.bin.part"
}

# ---- sheet ids and score -------------------------------------------------

test_sheet_key_and_page_share_one_id() {
  sheet_sandbox
  status "$root/bench/sheet" "$S/runs/zturbo-secretname-q4" "$S/runs/klein-hidden-q8"
  eq "$rc" 0 "exit code"
  local id page
  id=$(sed -n '1s/^# sheet //p' "$LIMGEN_BENCH_OUT/sheets/key.tsv")
  [[ $id =~ ^[0-9a-f]+$ ]] || fail "no sheet id on key line 1"
  page=$(cat "$LIMGEN_BENCH_OUT/sheets/index.html")
  contains "$page" "const SHEET = \"$id\";"
  contains "$page" '"limgen-l0-ratings-" + SHEET'
  contains "$page" '"# sheet " + SHEET'
}

test_sheet_two_sheets_get_different_ids() {
  sheet_sandbox
  local a b
  status "$root/bench/sheet" "$S/runs/zturbo-secretname-q4" "$S/runs/klein-hidden-q8"
  a=$(sed -n 1p "$LIMGEN_BENCH_OUT/sheets/key.tsv")
  status "$root/bench/sheet" "$S/runs/zturbo-secretname-q4" "$S/runs/klein-hidden-q8"
  b=$(sed -n 1p "$LIMGEN_BENCH_OUT/sheets/key.tsv")
  [[ $a != "$b" ]] || fail "two sheets both got '$a'"
}

test_score_ratings_from_another_sheet_are_refused() {
  score_sandbox
  sed -i '1s/.*/# sheet deadbeef/' "$S/ratings.tsv"
  status "$root/bench/score" "$S/ratings.tsv" "$S/key.tsv"
  eq "$rc" 1 "exit code"
  lacks "$out" runX
  lacks "$out" runY
  contains "$out" deadbeef
}

test_score_ratings_without_sheet_id_are_refused() {
  score_sandbox
  sed -i '1d' "$S/ratings.tsv"
  status "$root/bench/score" "$S/ratings.tsv" "$S/key.tsv"
  eq "$rc" 1 "exit code"
  lacks "$out" runX
}

test_score_key_without_sheet_id_is_refused() {
  score_sandbox
  sed -i '1d' "$S/key.tsv"
  status "$root/bench/score" "$S/ratings.tsv" "$S/key.tsv"
  eq "$rc" 1 "exit code"
  lacks "$out" runX
}

# ---- bench/run with a fake engine ----------------------------------------

# run_sandbox builds, under $R, dummy model files, a fake sd-cli that prints
# $FAKE_LOG and writes the -o files, and a fake nvidia-smi on PATH. The fake
# card has 8000 MiB, 2000 in use, and samples of 3000, 5000 and 4000.
run_sandbox() {
  R=$(mktemp -d "$T/run.XXXXXX")
  M=$R/models
  touch_files "${zimage_q4_files[@]}"
  mkdir -p "$R/bin" "$R/out"
  cat >"$R/bin/nvidia-smi" <<'FAKE'
#!/usr/bin/env bash
case "$*" in
  *memory.total*) echo 8000 ;;
  *-lms*) echo 3000; echo 5000; echo 4000; exec sleep 1000 ;;
  *memory.used*) echo 2000 ;;
esac
FAKE
  cat >"$R/sd-cli" <<'FAKE'
#!/usr/bin/env bash
out= n=1
while [[ $# -gt 0 ]]; do
  case $1 in -o) out=$2; shift 2 ;; -b) n=$2; shift 2 ;; *) shift ;; esac
done
sleep 0.3 # lets the memory sampler write its first lines
cat "$FAKE_LOG"
for ((i = 0; i < n; i++)); do : >"${out//%d/$i}"; done
exit "${FAKE_EXIT:-0}"
FAKE
  chmod +x "$R/bin/nvidia-smi" "$R/sd-cli"
  export PATH=$R/bin:$PATH LIMGEN_MODELS=$M LIMGEN_BENCH_OUT=$R/out LIMGEN_SD_CLI=$R/sd-cli
  export FAKE_LOG=$root/tests/data/sd-cli-z-image-batch4.log
  first_id=$(prompt_ids | sed -n 1p)
}

# runs_row <field> prints one field of the last row of runs.tsv.
runs_row() { tail -n 1 "$R/out/runs.tsv" | cut -f"$1"; }

test_run_end_to_end_records_a_row_from_the_log() {
  run_sandbox
  status "$root/bench/run" z-image-turbo --only "$first_id" --name t
  eq "$rc" 0 "exit code"
  eq "$(wc -l <"$R/out/runs.tsv")" 2 "header plus one row"
  eq "$(tail -n 1 "$R/out/runs.tsv" | awk -F'\t' '{print NF}')" 16 "fields"
  eq "$(runs_row 8)" "$first_id" prompt
  eq "$(runs_row 9)" 4 count
  eq "$(runs_row 10)" 0 "exit column"
  eq "$(runs_row 12)" 7.55 "load seconds (4.33 + 3.00 + 0.22)"
  eq "$(runs_row 13)" 10.37 "sample seconds per image (41.49 / 4)"
  eq "$(runs_row 14)" 0.81 "decode seconds per image (3.25 / 4)"
  eq "$(runs_row 15)" 6000 "free MiB at start (8000 - 2000)"
  eq "$(runs_row 16)" 5000 "peak MiB"
  local i
  for i in 0 1 2 3; do exists "$R/out/t/$first_id-$i.png"; done
}

test_run_error_line_after_a_normal_log_counts_as_failure() {
  # Same log as the passing run plus one error line, so only the "[E]" check
  # separates this from the end-to-end test: sd-cli exits 0 in both.
  run_sandbox
  { cat "$FAKE_LOG"; echo "[E] something failed --- x.cpp:1"; } >"$R/error.log"
  export FAKE_LOG=$R/error.log
  status "$root/bench/run" z-image-turbo --only "$first_id" --name t
  eq "$rc" 0 "exit code of bench/run itself"
  eq "$(runs_row 10)" 1 "exit column"
}

test_run_engine_error_line_counts_as_failure_despite_exit_0() {
  # The real captured log of the pinned sd-cli failing an ESRGAN upscale: it
  # logs "[E] upscale failed", has no timing lines, and exits 0. The prompt
  # must still get a row, with exit 1, and the run must go on.
  run_sandbox
  export FAKE_LOG=$root/tests/data/sd-cli-esrgan-oom-exit0.log
  status "$root/bench/run" z-image-turbo --only "$first_id" --name t
  eq "$rc" 0 "exit code of bench/run itself"
  eq "$(runs_row 10)" 1 "exit column"
}

test_run_records_the_engines_own_exit_status() {
  run_sandbox
  export FAKE_EXIT=7
  status "$root/bench/run" z-image-turbo --only "$first_id" --name t
  eq "$(runs_row 10)" 7 "exit column"
}

test_run_hold_failing_at_once_exits_3_and_runs_nothing() {
  run_sandbox
  printf '#!/usr/bin/env bash\necho "no memory"; exit 1\n' >"$R/out/hold-vram"
  chmod +x "$R/out/hold-vram"
  status "$root/bench/run" z-image-turbo --only "$first_id" --name t --hold 1
  eq "$rc" 3 "exit code"
  contains "$out" "hold-vram failed"
  eq "$(wc -l <"$R/out/runs.tsv")" 1 "runs.tsv has the header only"
  eq "$(find "$R/out/t" -name '*.cmd' | wc -l)" 0 "prompts started"
}

test_run_hold_that_confirms_lets_the_run_proceed_and_is_killed() {
  run_sandbox
  cat >"$R/out/hold-vram" <<FAKE
#!/usr/bin/env bash
echo \$\$ >"$R/holder.pid"
echo "hold-vram: holding \$1 GiB"
exec sleep 300
FAKE
  chmod +x "$R/out/hold-vram"
  status "$root/bench/run" z-image-turbo --only "$first_id" --name t --hold 1
  eq "$rc" 0 "exit code"
  eq "$(runs_row 7)" 1 "hold_gib column"
  eq "$(runs_row 10)" 0 "exit column"
  local pid i
  pid=$(cat "$R/holder.pid")
  for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid"
    fail "holder $pid still running after the run"
  fi
}

test_run_bad_count_seed_hold_are_usage_errors() {
  local args
  for args in "--count 0" "--count x" "--seed x" "--seed -1" "--hold abc"; do
    # shellcheck disable=SC2086
    status "$root/bench/run" z-image-turbo $args
    eq "$rc" 2 "exit code for $args"
  done
}

test_run_tae_preview_without_the_tae_file_exits_3() {
  run_sandbox
  status "$root/bench/run" z-image-turbo --only "$first_id" --name t --preview tae
  eq "$rc" 3 "exit code"
  contains "$out" taef1.safetensors
  absent "$R/out/runs.tsv"
}

test_run_tae_preview_passes_the_tae_file_to_the_engine() {
  run_sandbox
  touch_files z-image-turbo/taef1.safetensors
  status "$root/bench/run" z-image-turbo --only "$first_id" --name t --preview tae
  eq "$rc" 0 "exit code"
  contains "$(cat "$R/out/t/$first_id.cmd")" "taesd-preview-only"
  contains "$(cat "$R/out/t/$first_id.cmd")" "taef1.safetensors"
}

# ---- bench/finish with a fake engine -------------------------------------

# finish_sandbox: a fake sd-cli that answers the three calls bench/finish
# makes, steered by environment variables:
#   FAKE_UPSCALE  ok (4x image), silent ("[E]" line, copies the input, exit 0),
#                 quiet (copies the input, says nothing), error (4x image but
#                 logs "[E]")
#   FAKE_IMG2IMG  ok (image of the asked size) or wrong (10x10)
#   FAKE_PROMPT   "none" makes the metadata call print no prompt
# Every call is appended to $FAKE_CALLS.
finish_sandbox() {
  R=$(mktemp -d "$T/finish.XXXXXX")
  M=$R/models
  touch_files "${zimage_q4_files[@]}" realesrgan-x4plus/RealESRGAN_x4plus.pth
  mkdir -p "$R/bin" "$R/drafts/runA"
  cat >"$R/bin/nvidia-smi" <<'FAKE'
#!/usr/bin/env bash
case "$*" in
  *-lms*) echo 3000; exec sleep 1000 ;;
esac
FAKE
  cat >"$R/sd-cli" <<'FAKE'
#!/usr/bin/env bash
echo "$*" >>"$FAKE_CALLS"
mode= in= out= w= h=
while [[ $# -gt 0 ]]; do
  case $1 in
    -M) mode=$2; shift 2 ;;
    -i | --image) in=$2; shift 2 ;;
    -o) out=$2; shift 2 ;;
    -W) w=$2; shift 2 ;;
    -H) h=$2; shift 2 ;;
    *) shift ;;
  esac
done
sleep 0.2
case $mode in
  metadata)
    if [[ ${FAKE_PROMPT:-ok} == none ]]; then echo '{"entries": []}'; exit 0; fi
    printf '%s\n' '{"entries": [{"data": {"text": "SDCPP: {\"prompt\": \"a test prompt\"}"}}]}'
    ;;
  upscale)
    case ${FAKE_UPSCALE:-ok} in
      ok) magick "$in" -resize 400% "$out" ;;
      silent) echo "[E] upscale failed"; cp "$in" "$out" ;;
      quiet) cp "$in" "$out" ;;
      error) echo "[E] upscale failed"; magick "$in" -resize 400% "$out" ;;
    esac
    ;;
  *)
    if [[ ${FAKE_IMG2IMG:-ok} == wrong ]]; then w=10 h=10; fi
    magick -size "${w}x${h}" xc:gray "$out"
    ;;
esac
FAKE
  chmod +x "$R/bin/nvidia-smi" "$R/sd-cli"
  magick -size 32x16 xc:red "$R/drafts/runA/x-0.png"
  export PATH=$R/bin:$PATH LIMGEN_MODELS=$M LIMGEN_BENCH_OUT=$R/out LIMGEN_SD_CLI=$R/sd-cli
  export FAKE_CALLS=$R/calls.log
  : >"$FAKE_CALLS"
  id=runA-x-0
  fin=$R/out/finish
}

# finish_row <method> <column> prints a column of the last row for a method.
finish_row() { awk -F'\t' -v m="$1" -v c="$2" '$5 == m {v = $c} END {print v}' "$R/out/finish.tsv"; }
finish_run() { status "$root/bench/finish" --model z-image-turbo --size 64x36 "$@" "$R/drafts/runA/x-0.png"; }
size_of() { magick identify -format '%wx%h' "$1"; }

test_finish_all_steps_succeed_and_use_the_draft_prompt() {
  finish_sandbox
  finish_run --tile 256
  eq "$rc" 0 "exit code"
  eq "$(finish_row esrgan 8)" 0 "esrgan exit"
  eq "$(finish_row img2img 8)" 0 "img2img exit"
  eq "$(finish_row both 8)" 0 "both exit"
  eq "$(size_of "$fin/$id-esrgan.png")" 64x36 "esrgan size"
  eq "$(size_of "$fin/$id-img2img.png")" 64x36 "img2img size"
  eq "$(size_of "$fin/$id-both.png")" 64x36 "both size"
  contains "$(cat "$FAKE_CALLS")" "-p a test prompt"
  contains "$(cat "$FAKE_CALLS")" "--upscale-tile-size 256"
}

test_finish_silent_upscale_failure_is_caught_and_skips_both() {
  finish_sandbox
  export FAKE_UPSCALE=silent
  finish_run
  eq "$rc" 0 "exit code"
  eq "$(finish_row esrgan 8)" 1 "esrgan exit"
  absent "$fin/$id-esrgan.png"
  eq "$(finish_row both 8)" "" "both row"
  eq "$(finish_row img2img 8)" 0 "img2img still runs"
}

test_finish_wrong_sized_upscale_is_caught_without_an_error_line() {
  # Only the size check can catch this one: the engine says nothing.
  finish_sandbox
  export FAKE_UPSCALE=quiet
  finish_run
  eq "$(finish_row esrgan 8)" 1 "esrgan exit"
  absent "$fin/$id-esrgan.png"
  eq "$(finish_row both 8)" "" "both row"
}

test_finish_error_line_is_caught_even_when_the_size_is_right() {
  # Only the "[E]" check can catch this one: the output is the right size.
  finish_sandbox
  export FAKE_UPSCALE=error
  finish_run
  eq "$(finish_row esrgan 8)" 1 "esrgan exit"
  absent "$fin/$id-esrgan.png"
  eq "$(finish_row both 8)" "" "both row"
}

test_finish_wrong_sized_img2img_is_a_failure() {
  finish_sandbox
  export FAKE_IMG2IMG=wrong
  finish_run
  eq "$(finish_row esrgan 8)" 0 "esrgan exit"
  eq "$(finish_row img2img 8)" 1 "img2img exit"
  # A failed step's output must not stay where later tools would read it.
  absent "$fin/$id-img2img.png"
}

test_finish_stale_esrgan_output_is_removed_when_the_step_fails() {
  finish_sandbox
  finish_run
  exists "$fin/$id-esrgan.png"
  export FAKE_UPSCALE=silent
  finish_run
  eq "$(finish_row esrgan 8)" 1 "esrgan exit on second run"
  absent "$fin/$id-esrgan.png"
  absent "$fin/$id-both.png"
}

test_finish_draft_without_a_prompt_exits_1() {
  finish_sandbox
  export FAKE_PROMPT=none
  finish_run
  eq "$rc" 1 "exit code"
  contains "$out" "no prompt in"
}

test_finish_bad_tile_is_a_usage_error() {
  finish_sandbox
  finish_run --tile 0
  eq "$rc" 2 "exit code"
}

# ---- packaging -----------------------------------------------------------

# pkgbuild_eval <expression> [VAR=value...]: the PKGBUILD sourced in a clean
# shell, with the expression evaluated after it. Sourcing only defines
# variables and functions; nothing is built or downloaded.
pkgbuild_eval() {
  local expr=$1
  shift
  env -u LIMGEN_ENGINES -u LIMGEN_SOURCE "$@" bash -c 'source "$1"; eval "$2"' _ "$root/pkgbuild/PKGBUILD" "$expr"
}

test_pkgbuild_is_valid_bash() {
  bash -n "$root/pkgbuild/PKGBUILD"
}

test_pkgbuild_default_packages_are_limgen_and_the_vulkan_engine() {
  eq "$(pkgbuild_eval 'echo "${pkgname[*]}"')" "limgen limgen-engine-vulkan" "pkgname"
  local makedepends
  makedepends=$(pkgbuild_eval 'echo "${makedepends[*]}"')
  contains " $makedepends " " vulkan-headers "
  lacks " $makedepends " " cuda "
  local sources
  sources=$(pkgbuild_eval 'echo "${source[*]}"')
  lacks "$sources" "cutlass"
  contains "$sources" "limgen-0.1.0.tar.gz"
}

test_pkgbuild_cuda_adds_its_package_dependency_and_cutlass() {
  eq "$(pkgbuild_eval 'echo "${pkgname[*]}"' LIMGEN_ENGINES="vulkan cuda")" \
    "limgen limgen-engine-vulkan limgen-engine-cuda" "pkgname"
  local makedepends sources
  makedepends=$(pkgbuild_eval 'echo "${makedepends[*]}"' LIMGEN_ENGINES="vulkan cuda")
  contains " $makedepends " " cuda "
  contains " $makedepends " " vulkan-headers "
  sources=$(pkgbuild_eval 'echo "${source[*]}"' LIMGEN_ENGINES="vulkan cuda")
  contains "$sources" "cutlass-"
  # Only CUDA: no Vulkan package.
  eq "$(pkgbuild_eval 'echo "${pkgname[*]}"' LIMGEN_ENGINES="cuda")" "limgen limgen-engine-cuda" "pkgname, cuda only"
}

test_pkgbuild_local_source_leaves_out_the_release_tarball() {
  local sources
  sources=$(pkgbuild_eval 'echo "${source[*]}"' LIMGEN_SOURCE=local)
  lacks "$sources" "limgen-0.1.0.tar.gz"
  contains "$sources" "stable-diffusion.cpp-"
  sources=$(pkgbuild_eval 'echo "${source[*]}"')
  contains "$sources" "limgen-0.1.0.tar.gz"
}

test_pkgbuild_has_one_checksum_per_source() {
  count_sources_and_sums() {
    local sources sums
    sources=$(pkgbuild_eval 'echo "${#source[@]}"' "$@")
    sums=$(pkgbuild_eval 'echo "${#sha256sums[@]}"' "$@")
    [[ $sources -ge 3 ]] || fail "only $sources sources for [$*]"
    eq "$sums" "$sources" "checksums for [$*]"
  }
  count_sources_and_sums
  count_sources_and_sums LIMGEN_ENGINES="vulkan cuda"
  count_sources_and_sums LIMGEN_SOURCE=local
  count_sources_and_sums LIMGEN_ENGINES=cuda LIMGEN_SOURCE=local
  # CUDA adds a source, and the local checkout removes one.
  eq "$(pkgbuild_eval 'echo "${#source[@]}"' LIMGEN_ENGINES="vulkan cuda")" "$(($(pkgbuild_eval 'echo "${#source[@]}"') + 1))" "cuda adds cutlass"
  eq "$(pkgbuild_eval 'echo "${#source[@]}"' LIMGEN_SOURCE=local)" "$(($(pkgbuild_eval 'echo "${#source[@]}"') - 1))" "local drops the tarball"
}

test_pkgbuild_checksums_are_all_hashes() {
  # Every source is pinned, the release tarball too since v0.1.0 was tagged.
  local line n=0
  while read -r line; do
    n=$((n + 1))
    [[ $line =~ ^[0-9a-f]{64}$ ]] || fail "not a SHA-256: $line"
  done < <(pkgbuild_eval 'printf "%s\n" "${sha256sums[@]}"' LIMGEN_ENGINES="vulkan cuda")
  [[ $n -ge 5 ]] || fail "only $n checksums"
  while read -r line; do
    [[ $line =~ ^[0-9a-f]{64}$ ]] || fail "not a SHA-256 with LIMGEN_SOURCE=local: $line"
  done < <(pkgbuild_eval 'printf "%s\n" "${sha256sums[@]}"' LIMGEN_SOURCE=local)
}

test_pkgbuild_needs_the_vulkan_loader_to_build() {
  # sd-cli links libvulkan.so, which none of the Vulkan headers packages
  # bring; a clean chroot would fail at link time without it.
  local makedepends
  makedepends=$(pkgbuild_eval 'echo " ${makedepends[*]} "')
  contains "$makedepends" " vulkan-icd-loader "
}

test_pkgbuild_ties_limgen_to_its_engine_version() {
  # limgen builds sd-cli arguments for one engine release, so a partial
  # upgrade must not pair a new limgen with an old engine.
  local depends provides
  depends=$(pkgbuild_eval 'install() { :; }; pkgdir=/nonexistent startdir=/nonexistent srcdir=/nonexistent; package_limgen; echo " ${depends[*]} "')
  contains "$depends" " limgen-engine=0.1.0 "
  for engine in vulkan cuda; do
    provides=$(pkgbuild_eval "install() { :; }; pkgdir=/nonexistent startdir=/nonexistent srcdir=/nonexistent; package_limgen-engine-$engine; echo \" \${provides[*]} \"")
    contains "$provides" " limgen-engine=0.1.0 "
  done
}

test_pkgbuild_needs_qt_6_8() {
  # The draft tiles use Image.retainWhileLoading, new in Qt 6.8; older Qt
  # would fail to load the grid.
  local depends makedepends
  depends=$(pkgbuild_eval 'install() { :; }; pkgdir=/nonexistent startdir=/nonexistent srcdir=/nonexistent; package_limgen; echo " ${depends[*]} "')
  makedepends=$(pkgbuild_eval 'echo " ${makedepends[*]} "')
  contains "$depends" " qt6-base>=6.8 "
  contains "$depends" " qt6-declarative>=6.8 "
  contains "$makedepends" " qt6-base>=6.8 "
  contains "$makedepends" " qt6-declarative>=6.8 "
}

test_pkgbuild_defines_a_package_function_per_package() {
  local pkg
  for pkg in limgen limgen-engine-vulkan limgen-engine-cuda; do
    pkgbuild_eval "declare -F package_$pkg >/dev/null" || fail "no package_$pkg()"
  done
  pkgbuild_eval 'declare -F build >/dev/null && declare -F prepare >/dev/null'
}

test_pkgbuild_refuses_an_unknown_engine_before_building() {
  # build() must stop at the unknown name; a cmake that records any call proves nothing ran.
  local bin=$T/fakebin-unknown calls=$T/unknown.calls
  mkdir -p "$bin"
  printf '#!/bin/sh\necho "$0 $*" >>"%s"\nexit 1\n' "$calls" >"$bin/cmake"
  chmod +x "$bin/cmake"
  status env PATH="$bin:$PATH" LIMGEN_ENGINES=rocm bash -c 'source "$1"; build' _ "$root/pkgbuild/PKGBUILD"
  eq "$rc" 1 "exit code"
  contains "$out" "unknown engine rocm"
  absent "$calls"
}

test_pkgbuild_keeps_build_paths_out_of_the_engine() {
  # The engine's sources name their own file paths in messages (__FILE__);
  # unmapped, the binary would carry the builder's home folder.
  local bin=$T/fakebin-prefix calls=$T/prefix.calls
  mkdir -p "$bin"
  printf '#!/bin/sh\necho "CFLAGS=$CFLAGS CXXFLAGS=$CXXFLAGS ARGS=$*" >>"%s"\nexit 1\n' "$calls" >"$bin/cmake"
  chmod +x "$bin/cmake"
  status env PATH="$bin:$PATH" CFLAGS=-O2 CXXFLAGS=-O2 srcdir=/build/src \
    bash -c 'source "$1"; build' _ "$root/pkgbuild/PKGBUILD"
  exists "$calls"
  local recorded
  recorded=$(head -1 "$calls")
  contains "$recorded" "CFLAGS=-O2 -ffile-prefix-map=/build/src="
  contains "$recorded" "CXXFLAGS=-O2 -ffile-prefix-map=/build/src="
}

test_pkgbuild_stamps_the_engine_with_its_release() {
  # Built from an archive, the engine cannot ask git for its version and
  # would call itself "unknown", which limgen refuses; the PKGBUILD names
  # the release instead, and keeps git from answering for a repository the
  # build folder happens to sit in.
  local bin=$T/fakebin-version calls=$T/version.calls tag commit
  mkdir -p "$bin"
  printf '#!/bin/sh\necho "ARGS=$*" >>"%s"\nexit 1\n' "$calls" >"$bin/cmake"
  chmod +x "$bin/cmake"
  status env PATH="$bin:$PATH" srcdir=/build/src bash -c 'source "$1"; build' _ "$root/pkgbuild/PKGBUILD"
  tag=$(sed -n 's/^_sdcpp_tag=//p' "$root/pkgbuild/PKGBUILD")
  commit=$(sed -n 's/^_sdcpp_commit=//p' "$root/pkgbuild/PKGBUILD")
  local recorded
  recorded=$(head -1 "$calls")
  contains "$recorded" "-DSDCPP_BUILD_VERSION=$tag "
  contains "$recorded" "-DSDCPP_BUILD_COMMIT=${commit:0:7} "
  contains "$recorded" "-DGIT_EXE=OFF "
}

test_pkgbuild_installs_what_the_desktop_file_names() {
  local pkgbuild
  pkgbuild=$(cat "$root/pkgbuild/PKGBUILD")
  contains "$pkgbuild" "/usr/bin/limgen"
  contains "$pkgbuild" "/usr/share/applications/limgen.desktop"
  contains "$pkgbuild" "/usr/share/icons/hicolor/scalable/apps/limgen.svg"
  contains "$pkgbuild" "/usr/lib/limgen/sd-cli"
  exists "$root/pkgbuild/limgen.desktop"
  exists "$root/pkgbuild/limgen.svg"
  exists "$root/LICENSE"
}

# ---- bin/build-engine and the pin ---------------------------------------

# A copy of the repository layout bin/build-engine needs, with fake git,
# cmake and ninja that only record that they were called and fail: nothing
# is cloned or built, whatever the script decides.
engine_layout() {
  L=$(mktemp -d "$T/layout.XXXXXX")
  mkdir -p "$L/bin" "$L/pkgbuild" "$L/fakebin"
  cp "$root/bin/build-engine" "$L/bin/build-engine"
  cp "$root/pkgbuild/PKGBUILD" "$L/pkgbuild/PKGBUILD"
  local tool
  for tool in git cmake ninja; do
    printf '#!/bin/sh\necho "%s $*" >>"%s"\nexit 1\n' "$tool" "$L/calls.log" >"$L/fakebin/$tool"
    chmod +x "$L/fakebin/$tool"
  done
}

run_build_engine() {
  status env PATH="$L/fakebin:$PATH" "$L/bin/build-engine" "$@"
}

test_build_engine_refuses_when_the_pkgbuild_has_no_pin() {
  engine_layout
  grep -v '^_sdcpp_tag=\|^_sdcpp_commit=' "$root/pkgbuild/PKGBUILD" >"$L/pkgbuild/PKGBUILD"
  run_build_engine vulkan
  eq "$rc" 1 "exit code"
  contains "$out" "no engine pin"
  contains "$out" "pkgbuild/PKGBUILD"
  absent "$L/calls.log"
  absent "$L/build"
}

test_build_engine_refuses_when_only_part_of_the_pin_is_there() {
  local drop
  for drop in '^_sdcpp_tag=' '^_sdcpp_commit='; do
    engine_layout
    grep -v "$drop" "$root/pkgbuild/PKGBUILD" >"$L/pkgbuild/PKGBUILD"
    run_build_engine vulkan
    eq "$rc" 1 "exit code without $drop"
    contains "$out" "no engine pin"
    absent "$L/calls.log"
  done
  # An empty value is no pin either.
  engine_layout
  sed 's/^_sdcpp_tag=.*/_sdcpp_tag=/' "$root/pkgbuild/PKGBUILD" >"$L/pkgbuild/PKGBUILD"
  run_build_engine cuda
  eq "$rc" 1 "exit code with an empty tag"
  contains "$out" "no engine pin"
  absent "$L/calls.log"
}

test_build_engine_takes_the_pin_from_the_pkgbuild() {
  engine_layout
  sed -e 's/^_sdcpp_tag=.*/_sdcpp_tag=master-9-abcdef0/' -e 's/^_sdcpp_commit=.*/_sdcpp_commit=0123456789abcdef0123456789abcdef01234567/' \
    "$root/pkgbuild/PKGBUILD" >"$L/pkgbuild/PKGBUILD"
  run_build_engine vulkan
  # The fake git fails the clone, which is all it is asked for: it must be of that tag.
  [[ $rc -ne 0 ]] || fail "the fake git failed, so the script must too"
  exists "$L/calls.log"
  local calls
  calls=$(cat "$L/calls.log")
  contains "$calls" "git clone"
  contains "$calls" "--branch master-9-abcdef0"
  lacks "$calls" "$(sed -n 's/^_sdcpp_tag=//p' "$root/pkgbuild/PKGBUILD")"
  lacks "$calls" "cmake"
}

test_build_engine_takes_a_quoted_pin() {
  # Arch PKGBUILDs often quote values; the quotes are not part of the tag.
  engine_layout
  sed -e "s/^_sdcpp_tag=.*/_sdcpp_tag='master-9-abcdef0'/" \
    -e 's/^_sdcpp_commit=.*/_sdcpp_commit="0123456789abcdef0123456789abcdef01234567"/' \
    "$root/pkgbuild/PKGBUILD" >"$L/pkgbuild/PKGBUILD"
  run_build_engine vulkan
  local calls
  calls=$(cat "$L/calls.log")
  contains "$calls" "--branch master-9-abcdef0 "
  lacks "$calls" "'"
}

test_engine_pin_in_the_pkgbuild_is_written_once() {
  # The pin lines appear once, and bin/build-engine carries no copy of them.
  eq "$(grep -c '^_sdcpp_tag=' "$root/pkgbuild/PKGBUILD")" 1 "tag lines"
  eq "$(grep -c '^_sdcpp_commit=' "$root/pkgbuild/PKGBUILD")" 1 "commit lines"
  local tag
  tag=$(sed -n 's/^_sdcpp_tag=//p' "$root/pkgbuild/PKGBUILD")
  lacks "$(cat "$root/bin/build-engine")" "$tag"
  [[ $tag =~ ^master-[0-9]+-[0-9a-f]{7}$ ]] || fail "unexpected tag shape: $tag"
  [[ $(sed -n 's/^_sdcpp_commit=//p' "$root/pkgbuild/PKGBUILD") =~ ^[0-9a-f]{40}$ ]] || fail "commit is not 40 hex digits"
  # The tag ends with the commit's first seven digits.
  [[ $(sed -n 's/^_sdcpp_commit=//p' "$root/pkgbuild/PKGBUILD") == "${tag##*-}"* ]] || fail "tag and commit disagree"
}

# ---- desktop file and icon -----------------------------------------------

test_desktop_file_names_the_app() {
  local file=$root/pkgbuild/limgen.desktop
  grep -qx 'Type=Application' "$file" || fail "Type"
  grep -qx 'Exec=limgen' "$file" || fail "Exec"
  grep -qx 'Icon=limgen' "$file" || fail "Icon"
  grep -qx 'StartupWMClass=limgen' "$file" || fail "StartupWMClass"
  grep -qx 'Terminal=false' "$file" || fail "Terminal"
  grep -q '^Name=Limgen$' "$file" || fail "Name"
  # The window's app id (main.cpp) is what StartupWMClass matches.
  contains "$(cat "$root/src/main.cpp")" 'setDesktopFileName("limgen")'
}

test_desktop_file_has_a_comment_for_every_translation() {
  local code locale
  for code in $(ls "$root/src/i18n" | sed -n 's/\.json$//p' | grep -v '^en$'); do
    locale=${code//-/_}
    grep -q "^Comment\[$locale\]=." "$root/pkgbuild/limgen.desktop" || fail "no Comment[$locale] for src/i18n/$code.json"
  done
}

test_desktop_file_validates() {
  command -v desktop-file-validate >/dev/null || { echo "    (desktop-file-validate is not installed; skipped)"; return 0; }
  status desktop-file-validate "$root/pkgbuild/limgen.desktop"
  eq "$rc" 0 "desktop-file-validate: $out"
}

test_icon_is_well_formed_xml_and_an_svg() {
  python3 - "$root/pkgbuild/limgen.svg" <<'PY'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
assert root.tag == "{http://www.w3.org/2000/svg}svg", root.tag
assert root.get("viewBox"), "no viewBox, so it does not scale"
assert len(list(root)) >= 1, "nothing drawn"
PY
}

# ---- go ------------------------------------------------------------------

for name in $(declare -F | awk '{print $3}' | grep '^test_'); do
  run_test "$name"
done

echo
echo "$passed passed, $failed failed"
[[ $failed -eq 0 ]]
