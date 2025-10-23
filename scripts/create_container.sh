#!/bin/bash

set -e

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: $0 [--local] - Gets Kvasir build environment Docker container"
    echo "  (default): Pull prebuilt container from DockerHub"
    echo "  --local:   Build container locally from Dockerfile"
    echo ""
    echo "This script builds/pulls:"
    echo "  - kvasirio/build_environment:latest"
    exit 0
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)
REPO_ROOT=$(cd -- "${SCRIPT_DIR}/.." &>/dev/null && pwd)

if [ "$1" = "--local" ]; then
    echo "Building build environment container locally..."
    echo ""

    # Check if buildx is available and set it up if needed
    if ! docker buildx version &>/dev/null; then
        echo "Error: docker buildx is not available."
        echo "Please install docker buildx or update to a newer version of Docker."
        echo "See: https://docs.docker.com/go/buildx/"
        exit 1
    fi

    # Create and use a buildx builder instance if one doesn't exist
    if ! docker buildx inspect kvasir-builder &>/dev/null; then
        echo "Creating buildx builder instance..."
        docker buildx create --name kvasir-builder --driver docker-container --bootstrap
    fi
    docker buildx use kvasir-builder

    echo "Building build environment container..."
    docker pull archlinux
    docker buildx build -f ${SCRIPT_DIR}/Dockerfile.build-env ${SCRIPT_DIR} \
        --tag kvasirio/build_environment:local \
        --load
    echo "✓ Build environment container built successfully (tagged as :local)"
    echo ""

    echo "Container built successfully!"
else
    echo "Pulling prebuilt container from DockerHub..."
    echo ""

    docker pull kvasirio/build_environment:latest
    echo "✓ Build environment container pulled successfully"
    echo ""

    echo "Container pulled successfully!"
fi
