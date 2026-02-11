#!/bin/sh -eu

# Usage: ./build.sh [--clean]
# --clean: Force rebuild without cache (slower but ensures fresh build)
# default: Use cache (faster, TensorFlow only downloaded once)

# Use docker if CRUNTIME is not set to something else (e.g. 'podman')
CRUNTIME=${CRUNTIME:-docker}
CACHE_FLAG=''
if [ "${1:-}" = '--clean' ]; then
	CACHE_FLAG='--no-cache'
	echo 'Clean build (no cache) - TensorFlow will be downloaded'
else
	echo 'Cached build - reusing TensorFlow layer'
fi

echo '
=== Building Container image ==='
$CRUNTIME build --progress=plain $CACHE_FLAG --build-arg CHIP=aarch64 . -t detectx

echo '
=== Extracting .eap file from container ==='
CONTAINER_ID=$($CRUNTIME create detectx)
$CRUNTIME cp "$CONTAINER_ID":/opt/app ./build
$CRUNTIME rm "$CONTAINER_ID"

echo '
=== Copying .eap file to current directory ==='
cp -v ./build/*.eap .

echo '
=== Cleaning up build directory ==='
rm -rf ./build

echo '
=== Build complete ==='
ls -lh ./*.eap
