#!/bin/bash
# Builds and runs scripts/gen_examples.c against Halcyon's objects.
set -e
cd "$(dirname "$0")/.."
source /home/z/my-project/scripts/hal-env.sh
cc -Isrc -std=c17 -g -fsanitize=address,undefined scripts/gen_examples.c \
   build/obj/core/*.o build/obj/io/*.o -lm \
   -L/home/z/my-project/fdk-install/lib -lfdk \
   -Wl,-rpath,/home/z/my-project/fdk-install/lib -o build/gen_examples
./build/gen_examples examples
