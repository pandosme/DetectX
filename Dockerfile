ARG ARCH=aarch64
ARG VERSION=12.8.0
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

# Install TensorFlow for model parameter extraction (CACHED)
RUN . /opt/venv/bin/activate && pip install --no-cache-dir tensorflow

#-------------------------------------------------------------------------------
# Stage 2: Build ACAP application
#-------------------------------------------------------------------------------
FROM tensorflow-base

WORKDIR /opt/app

# Copy application source, headers, and prebuilt libraries
COPY ./app .

# Ensure local lib and include directories exist (if not already present)
RUN mkdir -p lib include

# Extract model parameters using TensorFlow (generates model_params.h)
RUN . /opt/venv/bin/activate && python extract_model_params.py 'model/model.tflite'

# Build and package ACAP application with assets required by your app
ARG CHIP=
RUN . /opt/axis/acapsdk/environment-setup* && acap-build . \
    -a 'settings/settings.json' \
    -a 'settings/events.json' \
    -a 'settings/mqtt.json' \
    -a 'model/model.tflite' \
    -a 'model/labels.txt'
