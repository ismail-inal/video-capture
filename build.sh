#!/usr/bin/env bash

echo "building"
mkdir -p build
g++ src/main.cpp -o build/main.out \
	-O3 -ffast-math -march=native \
	-pthread -Wall -Wextra -pedantic -std=c++23 \
	-lavcodec -lavutil -lavformat -lswscale -lSDL2 

echo "running"
./build/main.out
