#!/bin/sh -eu

usage() {
	cat <<'EOF'
Usage: ./build.sh [--clean] [--target a8|a9|all]

With no target, both ARTPEC-8 and ARTPEC-9 packages are built.
EOF
}

CRUNTIME=${CRUNTIME:-docker}
CACHE_FLAG=''
TARGET='all'

while [ "$#" -gt 0 ]; do
	case "$1" in
		--clean)
			CACHE_FLAG='--no-cache'
			shift
			;;
		--target)
			[ "$#" -ge 2 ] || { echo 'Missing value for --target' >&2; usage >&2; exit 2; }
			TARGET=$2
			shift 2
			;;
		--target=*)
			TARGET=${1#*=}
			shift
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Unknown argument: $1" >&2
			usage >&2
			exit 2
			;;
	esac
done

case "$TARGET" in
	a8) TARGETS='a8' ;;
	a9) TARGETS='a9' ;;
	all) TARGETS='a8 a9' ;;
	*) echo "Invalid target: $TARGET" >&2; usage >&2; exit 2 ;;
esac

for target in $TARGETS; do
	for model in "app/model/model-${target}.tflite"; do
		if [ ! -f "$model" ]; then
			echo "Missing $model" >&2
			echo "Provide the chip-specific TFLite model before building target $target." >&2
			exit 1
		fi
	done
done

BUILD_DIR=$(mktemp -d)
CONTAINER_IDS=''
GENERATED=''

cleanup() {
	for container_id in $CONTAINER_IDS; do
		$CRUNTIME rm -f "$container_id" >/dev/null 2>&1 || true
	done
	rm -rf "$BUILD_DIR"
}
trap cleanup EXIT HUP INT TERM

if [ -n "$CACHE_FLAG" ]; then
	echo 'Clean build (no cache) - TensorFlow will be downloaded'
else
	echo 'Cached build - reusing TensorFlow layer'
fi

for target in $TARGETS; do
	image="detectx-${target}"
	target_dir="$BUILD_DIR/$target"
	mkdir -p "$target_dir"

	echo "
=== Building ARTPEC-${target#a} container image ==="
	$CRUNTIME build --progress=plain $CACHE_FLAG \
		--build-arg "TARGET_CHIP=$target" . -t "$image"

	echo "
=== Extracting ARTPEC-${target#a} EAP ==="
	container_id=$($CRUNTIME create "$image")
	CONTAINER_IDS="$CONTAINER_IDS $container_id"
	$CRUNTIME cp "$container_id":/opt/app/. "$target_dir"
	$CRUNTIME rm "$container_id" >/dev/null

	set -- "$target_dir"/*.eap
	if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
		echo "Expected exactly one EAP for $target, found $#" >&2
		exit 1
	fi

	package_name=${1##*/}
	package_stem=${package_name%_aarch64.eap}
	output="${package_stem}_artpec${target#a}.eap"
	staged_output="$BUILD_DIR/$output"
	cp "$1" "$staged_output"
	GENERATED="$GENERATED $staged_output"
done

echo '
=== Build complete ==='
for package in $GENERATED; do
	output=${package##*/}
	cp -v "$package" "$output"
	ls -lh "$output"
done
