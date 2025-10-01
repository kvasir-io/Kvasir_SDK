#!/bin/bash

set -e

if [ "$(id -u)" = "0" ]; then
    USER_ID=${LOCAL_USER_ID:-50000}
    GROUP_ID=${LOCAL_GROUP_ID:-50000}

    # Try to get existing group name, or create new one
    if getent group $GROUP_ID >/dev/null 2>&1; then
        GROUP_NAME=$(getent group $GROUP_ID | cut -d: -f1)
        echo "Reusing existing group: $GROUP_NAME (GID: $GROUP_ID)"
    else
        groupadd -r -g $GROUP_ID container_user
        GROUP_NAME=container_user
    fi

    # Try to get existing user name, or create new one
    if getent passwd $USER_ID >/dev/null 2>&1; then
        USER_NAME=$(getent passwd $USER_ID | cut -d: -f1)
        echo "Reusing existing user: $USER_NAME (UID: $USER_ID)"
    else
        useradd --shell /bin/fish -u $USER_ID -g $GROUP_ID -o -m container_user
        USER_NAME=container_user
    fi

    # Ensure home directory exists
    export HOME=/home/$USER_NAME
    [ -d "$HOME" ] || mkdir -p "$HOME"

    exec su-exec $USER_NAME "$@"
else
    # Already running as correct user just exec
    exec "$@"
fi
