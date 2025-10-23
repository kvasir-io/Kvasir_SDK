#!/bin/bash

set -e

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    echo "Usage: $0 [OPTIONS] [COMMAND]"
    echo "Options:"
    echo "  --local Use locally built image instead of pulling from DockerHub"
    echo ""
    echo "Commands:"
    echo "  start   Start persistent container"
    echo "  attach  Attach to running container"
    echo "  stop    Stop running container"
    exit 0
fi

# Check for --local flag
USE_LOCAL=false
if [ "$1" = "--local" ]; then
    USE_LOCAL=true
    shift
fi

# Get the directory where the script is run from (current working directory)
WORK_DIR="$(pwd)"
CONTAINER_NAME="kvasir-$(basename "${WORK_DIR}")"

# Determine which image to use
if [ "$USE_LOCAL" = true ]; then
    DOCKER_IMAGE="kvasirio/build_environment:local"
else
    DOCKER_IMAGE="kvasirio/build_environment:latest"
fi

if [ "$1" = "start" ]; then
    if docker ps -q -f name="${CONTAINER_NAME}" | grep -q .; then
        echo "Warning: Container '${CONTAINER_NAME}' is already running"
        exit 0
    fi

    # Only pull if not using local image
    if [ "$USE_LOCAL" = false ]; then
        echo "Pulling latest container image..."
        if ! docker pull "${DOCKER_IMAGE}"; then
            echo "Error: Failed to pull container image"
            exit 1
        fi
    else
        echo "Using local image: ${DOCKER_IMAGE}"
    fi

    echo "Starting container: ${CONTAINER_NAME}"
    echo "Mounting directory: ${WORK_DIR}"
    if ! docker run --rm -d --name "${CONTAINER_NAME}" -v "${WORK_DIR}/:/workspace/project" -e "LOCAL_USER_ID=$(id -u "$USER")" -e "LOCAL_GROUP_ID=$(id -g "$USER")" "${DOCKER_IMAGE}" sleep infinity; then
        echo "Error: Failed to start container"
        exit 1
    fi
    echo "Container started successfully"

elif [ "$1" = "attach" ]; then
    if ! docker ps -q -f name="${CONTAINER_NAME}" | grep -q .; then
        echo "Error: Container '${CONTAINER_NAME}' is not running. Start it first with: $0 start"
        exit 1
    fi

    echo "Attaching to container: ${CONTAINER_NAME}"
    if ! docker exec -it "${CONTAINER_NAME}" /workspace/project/kvasir/scripts/entrypoint.sh fish; then
        echo "Error: Failed to attach to container"
        exit 1
    fi

elif [ "$1" = "stop" ]; then
    if ! docker ps -aq -f name="${CONTAINER_NAME}" | grep -q .; then
        echo "Warning: Container '${CONTAINER_NAME}' does not exist"
        exit 0
    fi

    echo "Stopping container: ${CONTAINER_NAME}"
    if ! docker stop "${CONTAINER_NAME}"; then
        echo "Error: Failed to stop container"
        exit 1
    fi
    echo "Container stopped successfully"

else
    echo "Error: Invalid option. Use -h for help."
    exit 1
fi
