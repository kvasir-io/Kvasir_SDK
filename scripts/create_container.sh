#!/bin/bash

set -e

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: $0 [--local] - Gets Kvasir development Docker container"
    echo "  (default): Pull prebuilt container from DockerHub"
    echo "  --local:   Build container locally from Dockerfile"
    exit 0
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)

if [ "$1" = "--local" ]; then
    echo "Building container locally..."
    docker pull archlinux
    docker build ${SCRIPT_DIR} --tag kvasirio/build_environment:latest
else
    echo "Pulling prebuilt container from DockerHub..."
    docker pull kvasirio/build_environment:latest
fi
