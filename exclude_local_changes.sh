#!/bin/bash

# Script to exclude specific files from being committed to the repository
# These files may change locally but should never be committed

set -e

echo "Configuring Git to exclude specific files from commits..."

# Files and patterns to exclude
FILES_TO_EXCLUDE=(
    ".gitignore"
    "README.md"
    "app/model/model.tflite"
    "app/model/labels.txt"
    "app/manifest.json"
)

PATTERNS_TO_GITIGNORE=(
    "*.eap"
    "*.EAP"
)

# Add patterns to .gitignore if not already present
echo ""
echo "Updating .gitignore..."
for pattern in "${PATTERNS_TO_GITIGNORE[@]}"; do
    if ! grep -Fxq "$pattern" .gitignore 2>/dev/null; then
        echo "$pattern" >> .gitignore
        echo "  Added: $pattern"
    else
        echo "  Already in .gitignore: $pattern"
    fi
done

# For each specific file, use git update-index --assume-unchanged
echo ""
echo "Marking files to be ignored by Git (assume-unchanged)..."
for file in "${FILES_TO_EXCLUDE[@]}"; do
    if [ -f "$file" ]; then
        git update-index --assume-unchanged "$file" 2>/dev/null && \
            echo "  ✓ Ignoring changes to: $file" || \
            echo "  ⚠ Could not mark (may not be tracked): $file"
    else
        echo "  ⚠ File not found: $file"
    fi
done

# Handle all .eap and .EAP files
echo ""
echo "Marking .eap/.EAP files to be ignored..."
for eapfile in *.eap *.EAP; do
    if [ -f "$eapfile" ]; then
        git update-index --assume-unchanged "$eapfile" 2>/dev/null && \
            echo "  ✓ Ignoring changes to: $eapfile" || \
            echo "  ⚠ Could not mark (may not be tracked): $eapfile"
    fi
done

echo ""
echo "=========================================="
echo "Configuration complete!"
echo ""
echo "To undo this for a specific file, run:"
echo "  git update-index --no-assume-unchanged <filename>"
echo ""
echo "To see which files are marked as assume-unchanged:"
echo "  git ls-files -v | grep '^h'"
echo "=========================================="
