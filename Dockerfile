ARG ARCH=aarch64
ARG VERSION=3.5
ARG UBUNTU_VERSION=20.04
ARG REPO=axisecp
ARG SDK=acap-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

#-------------------------------------------------------------------------------
# Build ACAP application
#-------------------------------------------------------------------------------

WORKDIR /opt/app
COPY ./app .
ARG CHIP=

RUN . /opt/axis/acapsdk/environment-setup* && acap-build . \
    -a 'model/model.tflite'
