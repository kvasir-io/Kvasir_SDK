#!/bin/bash

set -e

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then echo "Usage: $0 - Builds Kvasir development Docker container"; exit 0; fi

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )

docker pull archlinux
docker build ${SCRIPT_DIR} --tag kvasir_fw_build
