#!/bin/sh -eu
#
# Export one model at several input resolutions.
#
#   ./export_resolutions.sh <weights.pt> <a8|a9> [calibration-dir] [WxH ...]
#
# Input resolution is the single biggest lever on both latency and detection
# range, and it is baked into the .tflite -- so choosing it is an EXPORT-time
# decision, not a build or runtime setting. The application introspects whatever
# model it is given, so any of these can be dropped in without code changes.
#
# Both dimensions must be a multiple of 32 (YOLOv8's stride-32 feature pyramid).
# A 16:9-shaped input wastes far fewer pixels than a square one on a 16:9 scene.

WEIGHTS=${1:?usage: $0 <weights.pt> <a8|a9> [calib-dir] [WxH ...]}
TARGET=${2:?usage: $0 <weights.pt> <a8|a9> [calib-dir] [WxH ...]}
CALIB=${3:-}
shift 3 2>/dev/null || shift $
SIZES=${*:-"416x256 640x384 800x480 960x544 1280x736"}

[ -n "$CALIB" ] || { echo "A calibration directory is required -- see docs." >&2; exit 2; }

PY=${PYTHON:-python3}
OUT=${OUTDIR:-exports}
mkdir -p "$OUT"

for size in $SIZES; do
	w=${size%x*}; h=${size#*x}
	echo "=== ${w}x${h} (${TARGET}) ==="
	$PY export_yolov8.py --target "$TARGET" --weights "$WEIGHTS" \
		--width "$w" --height "$h" \
		--calibration-dir "$CALIB" --calibration-images 128 \
		--labels app/model/labels.txt \
		--output "$OUT/model-${TARGET}-${w}x${h}.tflite"
done
echo
echo "Exported to $OUT/ :"
ls -la "$OUT"/*.tflite 2>/dev/null | awk '{printf "  %-44s %.1f MB\n",$9,$5/1048576}'
