ARG ARCH=aarch64
ARG VERSION=12.11.0
ARG UBUNTU_VERSION=24.04
ARG REPO=docker.io/axisecp
ARG SDK=acap-native-sdk

#-------------------------------------------------------------------------------
# Stage 1: TensorFlow environment (cached layer)
#-------------------------------------------------------------------------------
FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION} AS tensorflow-base

RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 \
    python3-pip \
    python3-venv \
    && rm -rf /var/lib/apt/lists/*

# Create a virtual environment for installations using pip
RUN python3 -m venv /opt/venv

# Install the TensorFlow version validated by this repository (CACHED)
RUN . /opt/venv/bin/activate && pip install --no-cache-dir tensorflow==2.21.0

#-------------------------------------------------------------------------------
# Stage 2: Build ACAP application
#-------------------------------------------------------------------------------
FROM tensorflow-base

WORKDIR /opt/app

# Copy application source, headers, and prebuilt libraries
COPY ./app .

# Ensure local lib and include directories exist (if not already present)
RUN mkdir -p lib include

# Select chip-specific models while preserving the runtime's canonical paths.
ARG TARGET_CHIP
RUN case "$TARGET_CHIP" in a8|a9) ;; *) echo 'TARGET_CHIP must be a8 or a9' >&2; exit 2 ;; esac \
    && cp "model/model-${TARGET_CHIP}.tflite" model/model.tflite \
    && if [ "$TARGET_CHIP" = a9 ]; then \
        python3 -c "import json; p='manifest.json'; d=json.load(open(p)); d['acapPackageConf']['setup']['compatibleOsVersions'][0]['min']='13.0'; open(p, 'w').write(json.dumps(d, indent=4) + '\\n')"; \
       fi

# Validate both models and generate model_params.h for the selected package.
RUN . /opt/venv/bin/activate \
    && python extract_model_params.py 'model/model.tflite' --target "$TARGET_CHIP"

# Build and package ACAP application with assets required by your app
RUN . /opt/axis/acapsdk/environment-setup* && acap-build . \
    -a 'settings/settings.json' \
    -a 'settings/events.json' \
    -a 'settings/mqtt.json' \
    -a 'model/model.tflite' \
    -a 'model/labels.txt'
