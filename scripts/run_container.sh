#!/bin/bash

set -e

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

${SCRIPT_DIR}/create_container.sh

base_cmd="-v ${SCRIPT_DIR}/../../:/project -e LOCAL_USER_ID=`id -u $USER`  -e LOCAL_GROUP_ID=`id -g $USER` kvasir_fw_build"

if [ "$#" -eq 1  ] && [ "$1" == "-it" ]; then
    docker run --rm -it ${base_cmd} fish
else
    docker run --rm ${base_cmd} /bin/sh -c "mkdir -p docker_build && env CC=clang CXX=clang++ cmake . -B docker_build && cmake --build docker_build --parallel $(nproc)"
fi
