#!/bin/bash

set -e

USER_ID=${LOCAL_USER_ID:-50000}
GROUP_ID=${LOCAL_GROUP_ID:-50000}

groupadd -r -g $GROUP_ID container_user
useradd --shell /bin/fish -u $USER_ID -g $GROUP_ID -o -m container_user

export HOME=/home/container_user

exec su-exec container_user "$@"
