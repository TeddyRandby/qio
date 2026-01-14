#!/usr/bin/bash

zig cc -Os -std=c23 -D_GNU_SOURCE -DQIO_LINUX -Iinclude --target=x86_64-linux-gnu main.c -o main

for file in examples/*; do
  name=$(basename "$file" ".c")
  zig cc -Os -std=c23 -D_GNU_SOURCE -DQIO_LINUX -Iinclude --target=x86_64-linux-gnu "$file" -o "$name"
done

zig cc -Os -std=c23 -D_GNU_SOURCE -DQIO_MACOS -Iinclude --target=x86_64-macos-none main.c -o main-macos

for file in examples/*; do
  name=$(basename "$file" ".c")
  zig cc -Os -std=c23 -D_GNU_SOURCE -DQIO_MACOS -Iinclude --target=x86_64-macos-none "$file" -o "$name-macos"
done

# Windows has to link with ws2 (winsock)
zig cc -g -std=c23 -lws2_32 -D_GNU_SOURCE -DQIO_WINDOWS -Iinclude --target=x86_64-windows-gnu main.c -o main-windows.exe

for file in examples/*; do
  name=$(basename "$file" ".c")
  zig cc -g -std=c23 -lws2_32 -D_GNU_SOURCE -DQIO_WINDOWS -Iinclude --target=x86_64-windows-gnu "$file" -o "$name-windows.exe"
done
