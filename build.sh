#!/bin/sh
# Cross-compile nest-picker for the Nest Gen 1 (ARMv7 hard-float, glibc 2.19).
# Uses an Ubuntu 14.04 container because its armhf cross toolchain ships the
# same glibc version as the thermostat, so the binary links against the
# device's own /lib/libc.so.6.
set -e
cd "$(dirname "$0")"
docker build -q -t nest-ui-xc build >/dev/null
mkdir -p out
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" nest-ui-xc \
    sh -c 'arm-linux-gnueabihf-gcc -std=gnu99 -D_GNU_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter \
        -march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=hard \
        -o out/nest-picker src/*.c -lm -lpthread'
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" nest-ui-xc arm-linux-gnueabihf-strip out/nest-picker
ls -la out/nest-picker
