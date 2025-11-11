#!/bin/env bash

SDK_DIR="$HOME/Downloads/OptiTrack_Camera_SDK_3.4.0_Beta1_Ubuntu/CameraSDK"
SDK_INCLUDE_DIR="${SDK_DIR}/include"
SDK_LIB_DIR="${SDK_DIR}/lib"

SOURCE_FILE="src/main.cpp"
EXECUTABLE_NAME="optitrack_recorder"
OUTPUT_DIR="build"
OUTPUT_PATH="${OUTPUT_DIR}/${EXECUTABLE_NAME}"


mkdir -p $OUTPUT_DIR

echo "tryna compile FN."

clang++ "$SOURCE_FILE" -o "$OUTPUT_PATH" \
    -O2 -march=native \
    -pthread -Wall -Wextra -pedantic -std=c++17 \
    -lavcodec -lavutil -lavformat -lswscale -lSDL2 \
    -I"$SDK_INCLUDE_DIR" \
    -L"$SDK_LIB_DIR" -lCameraLibrary \
    -Wl,-rpath,"$SDK_LIB_DIR"

echo "Done FN."
