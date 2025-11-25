#!/usr/bin/env bash

set -e

SDK_INCLUDE_DIR="/home/ayibogan/Downloads/CameraSDK/include/"
SDK_LIB_DIR="/home/ayibogan/Downloads/CameraSDK/lib/"

echo "building"
mkdir -p build
g++ src/main.cpp -o build/main.out \
	-O3 -ffast-math -march=native \
	-pthread -Wall -Wextra -pedantic -std=c++23 \
	-lavcodec -lavutil -lavformat -lswscale -lSDL2 \
    -I"$SDK_INCLUDE_DIR" \
    -L"$SDK_LIB_DIR" -llibCameraLibrary \
    -Wl,-rpath,"$SDK_LIB_DIR"

echo "running"
./build/main.out
