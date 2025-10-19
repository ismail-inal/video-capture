#!/usr/bin/env bash

if [ $# -lt 1 ]; then
	echo "please supply file name"
	exit 1
fi

filename="${1##*/}"
filename="${filename%.*}"
filename+=".out"

clang++ $1 -o "build/$filename" \
	-O3 -ffast-math -march=native \
	-pthread -Wall -Wextra -pedantic -std=c++23 \
	-lavcodec -lavutil -lavformat -lswscale -lSDL2

./build/$filename
