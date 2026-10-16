#!/bin/bash
#
# Install a trained model into the ACAP before building.
#
#   tools/install-model.sh <model-package-dir> [--size WxH] [--no-check]
#   tools/install-model.sh --a8 <a8.tflite> --a9 <a9.tflite> --labels <labels.txt> [--no-check]
#
# A model package is the folder training produces (see docs/MODEL_CONTRACT.md):
# labels.txt plus one .tflite per chip and input size, named with the chip and size,
# e.g. hagrid15-a8-1120x640.tflite / hagrid15-a9-1120x640.tflite. With several sizes
# in the package, choose one with --size.
#
# The files are copied to the names the build uses -- app/model/model-a8.tflite,
# app/model/model-a9.tflite and app/model/labels.txt -- and each model is checked
# against the model contract (tools/check-model.sh, needs Docker). app/model/SOURCE.md
# records where the bundled model came from. Then run ./build.sh.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL_DIR="$ROOT/app/model"
PKG='' SIZE='' CHECK=1 LABELS=''
declare -A SRC=()

usage() { sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
	case "$1" in
		--size) SIZE=${2:?--size needs WxH}; shift 2 ;;
		--a8|--a9) SRC[${1#--}]=${2:?$1 needs a file}; shift 2 ;;
		--labels) LABELS=${2:?--labels needs a file}; shift 2 ;;
		--no-check) CHECK=0; shift ;;
		-h|--help) usage; exit 0 ;;
		-*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
		*) PKG=$1; shift ;;
	esac
done

if [ -n "$PKG" ]; then
	[ -d "$PKG" ] || { echo "Not a directory: $PKG" >&2; exit 1; }
	PKG=$(cd "$PKG" && pwd)
	if [ -z "$LABELS" ]; then
		if [ -f "$PKG/labels.txt" ]; then LABELS="$PKG/labels.txt"
		else
			mapfile -t found < <(find "$PKG" -maxdepth 1 -name '*labels*.txt' | sort)
			[ ${#found[@]} -eq 1 ] || { echo "Expected labels.txt in $PKG" >&2; exit 1; }
			LABELS=${found[0]}
		fi
	fi
	for chip in a8 a9; do
		[ -n "${SRC[$chip]:-}" ] && continue
		mapfile -t found < <(find "$PKG" -maxdepth 1 -name '*.tflite' \
			\( -name "${chip}-*" -o -name "*-${chip}-*" -o -name "*-${chip}.tflite" -o -name "*_${chip}*" \) | sort)
		if [ -n "$SIZE" ]; then
			mapfile -t found < <(printf '%s\n' "${found[@]}" | grep -F -- "$SIZE" || true)
		fi
		if [ ${#found[@]} -gt 1 ]; then
			echo "Several ${chip} models in $PKG; choose one with --size:" >&2
			printf '  %s\n' "${found[@]##*/}" >&2
			exit 1
		fi
		[ ${#found[@]} -eq 1 ] && [ -n "${found[0]}" ] && SRC[$chip]=${found[0]}
	done
fi

[ -n "$LABELS" ] && [ -f "$LABELS" ] || { echo "No labels file given or found" >&2; usage >&2; exit 1; }
[ -n "${SRC[a8]:-}${SRC[a9]:-}" ] || { echo "No ARTPEC-8 or ARTPEC-9 model found${SIZE:+ for $SIZE}" >&2; exit 1; }

if [ "$CHECK" -eq 1 ]; then
	for chip in a8 a9; do
		[ -n "${SRC[$chip]:-}" ] || continue
		echo "== Checking ${SRC[$chip]##*/} for ARTPEC-${chip#a}"
		"$ROOT/tools/check-model.sh" "${SRC[$chip]}" "$LABELS" --target "$chip"
	done
fi

mkdir -p "$MODEL_DIR"
cp "$LABELS" "$MODEL_DIR/labels.txt"
for chip in a8 a9; do
	if [ -n "${SRC[$chip]:-}" ]; then
		cp "${SRC[$chip]}" "$MODEL_DIR/model-${chip}.tflite"
	else
		echo "WARNING: no ARTPEC-${chip#a} model given; app/model/model-${chip}.tflite is unchanged and may not match the new labels." >&2
	fi
done

{
	echo "# Bundled model"
	echo
	echo "Installed $(date -u +%Y-%m-%dT%H:%M:%SZ) by tools/install-model.sh."
	echo
	[ -n "$PKG" ] && echo "Package: \`$PKG\`${SIZE:+ (size $SIZE)}" && echo
	if [ -n "$PKG" ] && [ -f "$PKG/model.json" ]; then
		echo '```json'; cat "$PKG/model.json"; echo '```'; echo
	fi
	echo "| File | Source | SHA-256 |"
	echo "|---|---|---|"
	for chip in a8 a9; do
		f="$MODEL_DIR/model-${chip}.tflite"
		[ -f "$f" ] || continue
		echo "| model-${chip}.tflite | ${SRC[$chip]:-(unchanged)} | $(sha256sum "$f" | cut -c1-16)... |"
	done
	echo "| labels.txt | $LABELS | $(sha256sum "$MODEL_DIR/labels.txt" | cut -c1-16)... |"
	echo
	echo "Labels ($(grep -c . "$MODEL_DIR/labels.txt")): $(grep . "$MODEL_DIR/labels.txt" | paste -sd, - | sed 's/,/, /g')"
} > "$MODEL_DIR/SOURCE.md"

echo "Installed into app/model/:"
for chip in a8 a9; do [ -n "${SRC[$chip]:-}" ] && echo "  model-${chip}.tflite  <- ${SRC[$chip]}"; done
echo "  labels.txt           <- $LABELS ($(grep -c . "$MODEL_DIR/labels.txt") labels)"
echo "Next: ./build.sh"
