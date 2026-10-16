#!/bin/sh
#
# Check a model against the DetectX model contract (docs/MODEL_CONTRACT.md).
#
#   tools/check-model.sh <model.tflite> <labels.txt> --target a8|a9
#
# Runs the same check the build runs (app/extract_model_params.py) inside the build's
# Docker image, so nothing has to be installed on this machine besides Docker. The
# first run builds the image (a few minutes); later runs take seconds.
#
# Checks: uint8 input [1,H,W,3]; two uint8 outputs, coordinates [1,4,N] and scores
# [1,classes,N]; N matches strides 8/16/32 for H x W; class count = number of labels;
# per-tensor weights for ARTPEC-8, per-channel for ARTPEC-9.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CRUNTIME=${CRUNTIME:-docker}
IMAGE=detectx-modelcheck

[ $# -ge 4 ] && [ "$3" = "--target" ] || { sed -n '3,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
MODEL=$1 LABELS=$2 TARGET=$4
case "$TARGET" in a8|a9) ;; *) echo "--target must be a8 or a9" >&2; exit 2 ;; esac
[ -f "$MODEL" ] || { echo "No such model: $MODEL" >&2; exit 1; }
[ -f "$LABELS" ] || { echo "No such labels file: $LABELS" >&2; exit 1; }

if ! $CRUNTIME image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "Building the model-check image (once)..."
	$CRUNTIME build -q --target tensorflow-base -t "$IMAGE" "$ROOT" >/dev/null
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp "$MODEL" "$WORK/model.tflite"
cp "$LABELS" "$WORK/labels.txt"
cp "$ROOT/app/extract_model_params.py" "$WORK/check.py"
chmod -R a+rwX "$WORK"

status=0
$CRUNTIME run --rm -v "$WORK:/m:ro" "$IMAGE" sh -c \
	". /opt/venv/bin/activate && TF_CPP_MIN_LOG_LEVEL=3 python /m/check.py /m/model.tflite --target $TARGET --output /tmp/model_params.h" \
	>"$WORK/out" 2>&1 || status=$?
# Show the check's own lines and, on failure, only the reason -- not TensorFlow's
# start-up messages or the Python traceback.
if [ "$status" -eq 0 ]; then
	grep -e '^  - ' "$WORK/out" || true
else
	grep -E '^[A-Za-z]*(Error|Exception):' "$WORK/out" | sed -e 's#/m/model.tflite#'"$MODEL"'#' -e 's#/m/labels.txt#'"$LABELS"'#' || tail -5 "$WORK/out"
fi
if [ "$status" -ne 0 ]; then
	echo "FAILED: $MODEL does not meet the DetectX model contract for $TARGET" >&2
	exit "$status"
fi
echo "OK: $MODEL meets the DetectX model contract for $TARGET"
