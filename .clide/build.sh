#!/usr/bin/bash

mkdir -p build/

zig cc -std=c23 -D_GNU_SOURCE -DQIO_LINUX -Iinclude --target=x86_64-linux-gnu main.c -o main

for file in examples/*; do
  name=$(basename "$file" ".c")
  zig cc -std=c23 -D_GNU_SOURCE -DQIO_LINUX -Iinclude --target=x86_64-linux-gnu "$file" -o "$name"
done

zig cc -std=c23 -D_GNU_SOURCE -DQIO_MACOS -Iinclude --target=x86_64-macos-none main.c -o main-macos

for file in examples/*; do
  name=$(basename "$file" ".c")
  zig cc -std=c23 -D_GNU_SOURCE -DQIO_MACOS -Iinclude --target=x86_64-macos-none "$file" -o "$name-macos"
done
