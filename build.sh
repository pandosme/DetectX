#!/bin/bash

# Usage: ./build.sh [--clean]
# --clean: Force rebuild without cache (slower but ensures fresh build)
# default: Use cache (faster, TensorFlow only downloaded once)

CACHE_FLAG=""
if [ "$1" = "--clean" ]; then
    CACHE_FLAG="--no-cache"
    echo "Clean build (no cache) - TensorFlow will be downloaded"
else
    echo "Cached build - reusing TensorFlow layer"
fi

echo ""
echo "=== Building Docker image ==="
docker build --progress=plain $CACHE_FLAG --build-arg CHIP=aarch64 . -t detectx

echo ""
echo "=== Extracting .eap file from container ==="
CONTAINER_ID=$(docker create detectx)
docker cp $CONTAINER_ID:/opt/app ./build
docker rm $CONTAINER_ID

echo ""
echo "=== Copying .eap file to current directory ==="
cp -v ./build/*.eap .

echo ""
echo "=== Cleaning up build directory ==="
rm -rf ./build

echo ""
echo "=== Build complete ==="
ls -lh *.eap