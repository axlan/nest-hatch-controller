# Cross toolchain matching the Nest Gen1 userspace (glibc 2.19, armhf, kernel 2.6.37)
FROM ubuntu:14.04
RUN apt-get update && apt-get install -y --no-install-recommends gcc-arm-linux-gnueabihf libc6-dev-armhf-cross make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
