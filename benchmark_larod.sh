#!/bin/sh -eu
#
# Fast DLPU inference benchmark using larod-client over SSH.
#
#   SSH_USER=root SSH_PASS=... ./benchmark_larod.sh <camera> <chip> <dir-of-tflite>
#
# This times PURE DLPU inference (no capture, preprocessing or NMS) with random
# input, the same way axis-model-zoo does. It is seconds per model instead of a
# minute, so it sweeps a whole model-size x resolution matrix quickly.
#
# CAVEAT measured on ARTPEC-8: larod-client carries a fixed per-run overhead
# (~40 ms, host->DLPU buffer copy; dma-buf is not supported on A8) that a running
# ACAP amortizes with double-buffering. So these numbers are ACCURATE for
# COMPARING models at the same resolution (the overhead cancels) but PESSIMISTIC
# as absolute latency below ~1 Mpx. For true end-to-end latency use
# benchmark_resolutions.sh (drives the running app), which reports what fps
# actually depends on.

HOST=${1:?usage: $0 <camera> <chip:a8|a9> <dir>}
CHIP=${2:?}; DIR=${3:?}
U=${SSH_USER:?set SSH_USER}; P=${SSH_PASS:?set SSH_PASS}
DEV="axis-${CHIP}-dlpu-tflite"
SSH="sshpass -p $P ssh -o StrictHostKeyChecking=no -o ConnectTimeout=6 $U@$HOST"

printf '%-18s %-8s %s\n' "model" "input" "dlpu_ms"
for f in "$DIR"/*.tflite; do
	[ -e "$f" ] || continue
	base=$(basename "$f")
	sshpass -p "$P" scp -O -o StrictHostKeyChecking=no "$f" "$U@$HOST:/tmp/b.tflite" >/dev/null 2>&1
	t=$($SSH "larod-client -c $DEV -g /tmp/b.tflite -i '' -w 5 -R 30 2>&1 \
		| grep -oE 'Mean execution time for job: [0-9.]+' | grep -oE '[0-9.]+$'" 2>/dev/null)
	printf '%-18s %-8s %s\n' "$base" "" "${t:-FAILED}"
done
