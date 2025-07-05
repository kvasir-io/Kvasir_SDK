#!/bin/bash

set -e

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

docker pull archlinux
docker build ${SCRIPT_DIR} --tag kvasir_fw_build
