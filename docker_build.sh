#!/bin/sh

mkdir -p docker_build
cd docker_build
cmake ..
cmake --build . -j $(nproc)
