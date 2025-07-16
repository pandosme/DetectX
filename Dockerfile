ARG ARCH=aarch64
ARG VERSION=12.5.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

WORKDIR /opt/app

# Copy application source, headers, and prebuilt libraries
COPY ./app .

# Ensure local lib and include directories exist (if not already present)
RUN mkdir -p lib include

# (Optional) If your build system or ACAP package expects them, you can re-copy or symlink .so files here.

# Build and package ACAP application with assets required by your app
ARG CHIP=
RUN . /opt/axis/acapsdk/environment-setup* && acap-build . \
    -a 'settings/settings.json' \
    -a 'settings/events.json' \
    -a 'settings/mqtt.json' \
    -a 'model/model.tflite' \
    -a 'model/model.json'
