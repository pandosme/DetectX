#!/bin/bash
#
# Install DetectX EAP to Axis camera
#

CAMERA_HOST="${1:-front.internal}"
USERNAME="nodered"
PASSWORD="rednode"

# Find the first .eap file in the current directory
EAP_FILE=$(ls -1 *.eap 2>/dev/null | head -n 1)

if [ -z "$EAP_FILE" ]; then
    echo "ERROR: No .eap file found in the current directory"
    exit 1
fi

echo "Installing $EAP_FILE to $CAMERA_HOST..."

# Upload using curl with digest authentication
curl --digest -u "$USERNAME:$PASSWORD" \
    -F "packfil=@$EAP_FILE;type=application/octet-stream" \
    "http://$CAMERA_HOST/axis-cgi/applications/upload.cgi"

echo ""
echo "Done."
