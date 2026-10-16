#!/bin/bash
#
# Manage DetectX on a camera from the command line (VAPIX, HTTP digest).
#
#   AXIS_USER=<user> AXIS_PASS=<password> tools/camera.sh <camera> <command> [args]
#
# Commands:
#   install <file.eap>                upload and install a package (then starts it)
#   start | stop | restart            control the application
#   status                            application state and model status (FPS, inference time)
#   log [lines]                       the application's lines from the camera's system log (default 50)
#   upload-model <model.tflite> [labels.txt]
#                                     replace the model at runtime, no rebuild (the app restarts);
#                                     the model must be exported for this camera's chip
#
# <camera> is a host name or IP address. HTTPS: set AXIS_SCHEME=https (add AXIS_INSECURE=1
# for a self-signed certificate). The application name is read from app/manifest.json
# (override with APP=<name>).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
usage() { sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'; }
[ $# -ge 2 ] || { usage >&2; exit 2; }
HOST=$1 CMD=$2; shift 2
: "${AXIS_USER:?set AXIS_USER}" "${AXIS_PASS:?set AXIS_PASS}"
SCHEME=${AXIS_SCHEME:-http}
APP=${APP:-$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['acapPackageConf']['setup']['appName'])" "$ROOT/app/manifest.json")}
BASE="$SCHEME://$HOST"
CURL=(curl -sS --fail --digest -u "$AXIS_USER:$AXIS_PASS" --max-time 120)
[ "${AXIS_INSECURE:-0}" = 1 ] && CURL+=(-k)

control() { "${CURL[@]}" "$BASE/axis-cgi/applications/control.cgi?action=$1&package=$APP"; echo; }

case "$CMD" in
	install)
		EAP=${1:?install needs an .eap file}
		[ -f "$EAP" ] || { echo "No such file: $EAP" >&2; exit 1; }
		echo "Uploading ${EAP##*/} to $HOST..."
		"${CURL[@]}" --max-time 600 -F "packfil=@$EAP" "$BASE/axis-cgi/applications/upload.cgi"; echo
		control start ;;
	start|stop|restart)
		control "$CMD" ;;
	status)
		"${CURL[@]}" "$BASE/axis-cgi/applications/list.cgi" | python3 -c '
import sys, xml.etree.ElementTree as ET
app = sys.argv[1]
for a in ET.fromstring(sys.stdin.read()).iter("application"):
    if a.get("Name") == app:
        print("%s %s: %s" % (a.get("NiceName") or app, a.get("Version", "?"), a.get("Status", "?")))
        break
else:
    print(app + " is not installed")' "$APP"
		"${CURL[@]}" "$BASE/local/$APP/status" 2>/dev/null | python3 -c '
import json, sys
try:
    m = json.load(sys.stdin).get("model", {})
except Exception:
    sys.exit(0)
print("model: %s, %s fps, inference %s ms%s" % (m.get("status", "?"), m.get("fps", "-"),
      m.get("averageTime", "-"), (", error: " + m["error"]) if m.get("error") else ""))' || true ;;
	log)
		"${CURL[@]}" "$BASE/axis-cgi/admin/systemlog.cgi" | grep -iE "\b$APP\b|sdk$APP" | tail -n "${1:-50}" ;;
	upload-model)
		MODEL=${1:?upload-model needs a .tflite file}; LABELS=${2:-}
		[ -f "$MODEL" ] || { echo "No such file: $MODEL" >&2; exit 1; }
		BODY=$(mktemp); trap 'rm -f "$BODY"' EXIT
		python3 - "$MODEL" "$LABELS" > "$BODY" <<'PYEOF'
import base64, json, sys
model, labels = sys.argv[1], sys.argv[2]
body = {"tflite_b64": base64.b64encode(open(model, "rb").read()).decode()}
if labels:
    body["labels_content"] = open(labels, encoding="utf-8").read()
json.dump(body, sys.stdout)
PYEOF
		"${CURL[@]}" -H 'Content-Type: application/json' --data-binary "@$BODY" "$BASE/local/$APP/model"; echo
		echo "Uploaded; $APP restarts and loads the model (30-90 s). Follow with: tools/camera.sh $HOST status" ;;
	-h|--help|help) usage ;;
	*) echo "Unknown command: $CMD" >&2; usage >&2; exit 2 ;;
esac
