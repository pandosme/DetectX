#!/bin/sh -eu
# Clean installation script for DetectX
# Run this script on the camera via SSH

echo '=========================================
DetectX Clean Installation Script
========================================='

# Stop the application
echo '
1. Stopping detectx application...'
systemctl stop detectx 2>/dev/null || true

# Check if package exists
if [ -d '/usr/local/packages/detectx' ]; then
	echo '
2. Found existing installation. Checking HTML file...'

	# Check current transparency value
	if [ -f '/usr/local/packages/detectx/html/index.html' ]; then
		echo '   Current transparency value:'
		grep 'rgba(0, 0, 0,' /usr/local/packages/detectx/html/index.html | grep fillStyle | head -1

		echo '   Current card width:'
		grep 'control-row mt-4' /usr/local/packages/detectx/html/index.html
	fi

	# Complete removal
	echo '
3. Completely removing old installation...'
	rm -rf /usr/local/packages/detectx

	# Verify removal
	if [ -d '/usr/local/packages/detectx' ]; then
		echo '   ERROR: Failed to remove old installation!' >&2
		exit 1
	else
		echo '   Successfully removed old installation'
	fi
else
	echo '
2. No existing installation found'
fi

# Install new version
echo '
4. Installing new version...'
if [ ! -f '/tmp/DetectX_COCO_3_6_0_aarch64.eap' ]; then
	echo '   ERROR: /tmp/DetectX_COCO_3_6_0_aarch64.eap not found!
Please copy the .eap file to /tmp first:
scp DetectX_COCO_3_6_0_aarch64.eap root@<camera-ip>:/tmp/' >&2
	exit 1
fi

cd /tmp
tar -xzf DetectX_COCO_3_6_0_aarch64.eap

# Find and run the install script
INSTALL_SCRIPT=$(find . -maxdepth 1 -name 'detectx_*.sh' -print -quit 2>/dev/null)
if [ -z "$INSTALL_SCRIPT" ]; then
	echo '   ERROR: Install script not found!' >&2
	exit 1
fi

echo "   Running $INSTALL_SCRIPT..."
chmod +x "$INSTALL_SCRIPT"
./"$INSTALL_SCRIPT" install

# Verify installation
echo '
5. Verifying new installation...'
if [ -d '/usr/local/packages/detectx' ]; then
	echo '   Installation directory exists: OK'

	if [ -f '/usr/local/packages/detectx/html/index.html' ]; then
		echo '
   Checking HTML file in installed package:
   Transparency value:'
		grep 'rgba(0, 0, 0,' /usr/local/packages/detectx/html/index.html | grep fillStyle | head -1

		echo '
   Card width:'
		grep 'control-row mt-4' /usr/local/packages/detectx/html/index.html

		echo '
   Scale mode options:'
		grep -A2 settings_scaleMode /usr/local/packages/detectx/html/index.html | grep 'option value'
	else
		echo '   ERROR: index.html not found in installation!' >&2
		exit 1
	fi
else
	echo '   ERROR: Installation failed!' >&2
	exit 1
fi

# Start the application
echo '
6. Starting detectx application...'
systemctl start detectx

# Wait a moment
sleep 2

# Check status
echo '
7. Application status:'
systemctl status detectx --no-pager | head -10

echo '
=========================================
Installation complete!
=========================================

IMPORTANT: Clear your browser cache or use incognito mode:
  - Firefox: Ctrl+Shift+Delete
  - Chromium: Ctrl+Shift+Delete
  - Or open in incognito/private mode

Then navigate to: http://<camera-ip>/local/detectx/index.html
'
