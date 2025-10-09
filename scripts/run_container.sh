#!/bin/bash

set -e

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: $0 [-it] - Builds project (default) or opens interactive shell (-it)"
    exit 0
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)

# Ensure we have the latest container image
docker pull kvasirio/build_environment:latest

base_cmd="-v ${SCRIPT_DIR}/../../:/workspace/project -e LOCAL_USER_ID=$(id -u $USER)  -e LOCAL_GROUP_ID=$(id -g $USER) kvasirio/build_environment:latest"

if [ "$#" -eq 1 ] && [ "$1" == "-it" ]; then
    docker run --rm -it ${base_cmd} fish
else
    docker run --rm ${base_cmd} /bin/sh -c "mkdir -p docker_build && env CC=clang CXX=clang++ cmake . -B docker_build && cmake --build docker_build --parallel $(nproc)"
fi
