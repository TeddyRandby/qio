#!/usr/bin/bash

mkdir -p build/

zig cc -std=c23 -D_GNU_SOURCE -DQIO_LINUX --target=x86_64-linux-gnu main.c -o main

for file in examples/*; do
  name=$(basename "$file" ".c")
  zig cc -std=c23 -D_GNU_SOURCE -DQIO_LINUX --target=x86_64-linux-gnu "$file" -o "$name"
done
