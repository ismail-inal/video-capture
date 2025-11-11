#!/bin/env bash
set -e # Exit immediately if a command exits with a non-zero status.

# --- Configuration ---
# 1. This is the path you provided.
SDK_DIR="$HOME/Downloads/OptiTrack_Camera_SDK_3.4.0_Beta1_Ubuntu/CameraSDK"

# 2. This is the source file we've been working on.
SOURCE_FILE="src/main.cpp"

# 3. This will be the final executable name.
EXECUTABLE_NAME="optitrack_recorder"
# ---------------------

# --- Automatic Paths ---
SDK_INCLUDE_DIR="${SDK_DIR}/include"
SDK_LIB_DIR="${SDK_DIR}/lib"
OUTPUT_DIR="build"
OUTPUT_PATH="${OUTPUT_DIR}/${EXECUTABLE_NAME}"

# Create build directory
mkdir -p $OUTPUT_DIR

echo "--- Compiling ${SOURCE_FILE} ---"

clang++ "$SOURCE_FILE" -o "$OUTPUT_PATH" \
    `# --- Your optimizations and warnings ---` \
    -O2 -march=native \
    -pthread -Wall -Wextra -pedantic -std=c++23 \
    \
    `# --- FFmpeg & SDL Libraries ---` \
    -lavcodec -lavutil -lavformat -lswscale -lSDL2 \
    \
    `# --- OptiTrack SDK ---` \
    -I"$SDK_INCLUDE_DIR" \
    -L"$SDK_LIB_DIR" -lCameraLibrary \
    \
    `# --- Linker setting to find the OptiTrack .so file at runtime ---` \
    -Wl,-rpath,"$SDK_LIB_DIR"

echo "--- Success! ---"
echo "Executable created at: ${OUTPUT_PATH}"
echo "Run with: ./${OUTPUT_PATH}"
