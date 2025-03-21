#!/usr/bin/bash

mkdir -p build/

zig cc -std=c23 -D_GNU_SOURCE -DQIO_LINUX --target=x86_64-linux-gnu main.c -o main
