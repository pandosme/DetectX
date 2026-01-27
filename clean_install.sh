#!/bin/bash
# Clean installation script for DetectX
# Run this script on the camera via SSH

echo "========================================="
echo "DetectX Clean Installation Script"
echo "========================================="

# Stop the application
echo ""
echo "1. Stopping detectx application..."
systemctl stop detectx 2>/dev/null || true

# Check if package exists
if [ -d "/usr/local/packages/detectx" ]; then
    echo ""
    echo "2. Found existing installation. Checking HTML file..."

    # Check current transparency value
    if [ -f "/usr/local/packages/detectx/html/index.html" ]; then
        echo "   Current transparency value:"
        grep "rgba(0, 0, 0," /usr/local/packages/detectx/html/index.html | grep "fillStyle" | head -1

        echo "   Current card width:"
        grep "control-row mt-4" /usr/local/packages/detectx/html/index.html
    fi

    # Complete removal
    echo ""
    echo "3. Completely removing old installation..."
    rm -rf /usr/local/packages/detectx

    # Verify removal
    if [ -d "/usr/local/packages/detectx" ]; then
        echo "   ERROR: Failed to remove old installation!"
        exit 1
    else
        echo "   Successfully removed old installation"
    fi
else
    echo ""
    echo "2. No existing installation found"
fi

# Install new version
echo ""
echo "4. Installing new version..."
if [ ! -f "/tmp/DetectX_COCO_3_6_0_aarch64.eap" ]; then
    echo "   ERROR: /tmp/DetectX_COCO_3_6_0_aarch64.eap not found!"
    echo "   Please copy the .eap file to /tmp first:"
    echo "   scp DetectX_COCO_3_6_0_aarch64.eap root@<camera-ip>:/tmp/"
    exit 1
fi

cd /tmp
gunzip -c DetectX_COCO_3_6_0_aarch64.eap | tar -xf -

# Find and run the install script
INSTALL_SCRIPT=$(ls -1 detectx_*.sh 2>/dev/null | head -1)
if [ -z "$INSTALL_SCRIPT" ]; then
    echo "   ERROR: Install script not found!"
    exit 1
fi

echo "   Running $INSTALL_SCRIPT..."
chmod +x "$INSTALL_SCRIPT"
./"$INSTALL_SCRIPT" install

# Verify installation
echo ""
echo "5. Verifying new installation..."
if [ -d "/usr/local/packages/detectx" ]; then
    echo "   Installation directory exists: OK"

    if [ -f "/usr/local/packages/detectx/html/index.html" ]; then
        echo ""
        echo "   Checking HTML file in installed package:"
        echo "   Transparency value:"
        grep "rgba(0, 0, 0," /usr/local/packages/detectx/html/index.html | grep "fillStyle" | head -1

        echo ""
        echo "   Card width:"
        grep "control-row mt-4" /usr/local/packages/detectx/html/index.html

        echo ""
        echo "   Scale mode options:"
        grep -A2 "settings_scaleMode" /usr/local/packages/detectx/html/index.html | grep "option value"
    else
        echo "   ERROR: index.html not found in installation!"
        exit 1
    fi
else
    echo "   ERROR: Installation failed!"
    exit 1
fi

# Start the application
echo ""
echo "6. Starting detectx application..."
systemctl start detectx

# Wait a moment
sleep 2

# Check status
echo ""
echo "7. Application status:"
systemctl status detectx --no-pager | head -10

echo ""
echo "========================================="
echo "Installation complete!"
echo "========================================="
echo ""
echo "IMPORTANT: Clear your browser cache or use incognito mode:"
echo "  - Firefox: Ctrl+Shift+Delete"
echo "  - Chromium: Ctrl+Shift+Delete"
echo "  - Or open in incognito/private mode"
echo ""
echo "Then navigate to: http://<camera-ip>/local/detectx/index.html"
echo ""
