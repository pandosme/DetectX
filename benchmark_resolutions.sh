#!/bin/sh -eu
#
# Measure inference time for each exported resolution on a real camera.
#
#   AXIS_USER=u AXIS_PASS=p ./benchmark_resolutions.sh <camera-host> [exports-dir]
#
# Uploads each model through the application's own /model endpoint (which stores
# it as a custom model and restarts), waits for the model to come up, then reads
# the average inference time the application reports. No rebuild per resolution.

HOST=${1:?usage: $0 <camera-host> [exports-dir]}
DIR=${2:-exports}
USER=${AXIS_USER:?set AXIS_USER}
PASS=${AXIS_PASS:?set AXIS_PASS}
PY=${PYTHON:-python3}

printf '%-14s %-8s %-9s %s\n' "resolution" "Mpx" "time" "fps"
for f in "$DIR"/*.tflite; do
	name=$(basename "$f" .tflite); size=${name##*-}
	w=${size%x*}; h=${size#*x}

	"$PY" - "$f" "$HOST" "$USER" "$PASS" <<'PYEOF'
import base64, json, sys, urllib.request
f, host, user, pw = sys.argv[1:5]
mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
mgr.add_password(None, f"http://{host}/", user, pw)
op = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(mgr))
body = json.dumps({"tflite_b64": base64.b64encode(open(f,"rb").read()).decode()}).encode()
req = urllib.request.Request(f"http://{host}/local/detectx/model", data=body,
                             headers={"Content-Type": "application/json"}, method="POST")
op.open(req, timeout=120).read()
PYEOF

	# the application restarts itself after a model upload; wait for it to load
	avg=""; i=0
	while [ $i -lt 40 ]; do
		sleep 5; i=$((i+1))
		avg=$("$PY" - "$HOST" "$USER" "$PASS" <<'PYEOF' || true
import json, sys, urllib.request
host, user, pw = sys.argv[1:4]
mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
mgr.add_password(None, f"http://{host}/", user, pw)
op = urllib.request.build_opener(urllib.request.HTTPDigestAuthHandler(mgr))
try:
    m = json.load(op.open(f"http://{host}/local/detectx/status", timeout=8))["model"]
    print(m.get("averageTime","") if m.get("state") else "")
except Exception:
    print("")
PYEOF
)
		[ -n "$avg" ] && break
	done
	mpx=$(awk "BEGIN{printf \"%.2f\", $w*$h/1000000}")
	fps=$([ -n "$avg" ] && awk "BEGIN{printf \"%.1f\", 1000/$avg}" || echo "-")
	printf '%-14s %-8s %-9s %s\n' "${w}x${h}" "$mpx" "${avg:-failed} ms" "$fps"
done
