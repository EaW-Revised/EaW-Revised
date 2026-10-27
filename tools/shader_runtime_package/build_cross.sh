#!/usr/bin/env bash
# Maintainer-only offline dependency build. Run on the supervised Linux CPU
# host; ARCHIVE is already fetched and must match the reviewed source hash.
# Usage: build_cross.sh ARCHIVE OUTPUT_DIRECTORY SLOT_HELPER QUEUE_HELPER
set -euo pipefail
archive=$(realpath "$1")
output=$(realpath -m "$2")
helper=$(realpath "$3")
queue_helper=$(realpath "$4")
expected=1333fd2a05ab8a0572106e3b7fb8161ea0b67ab7d0a1b8bdd14b47f89ac8a611
printf '%s  %s\n' "$expected" "$archive" | sha256sum -c -
command -v x86_64-w64-mingw32-g++-win32
base="$HOME/eawr-offload"
lock_dir=${EAWR_BUILD_LOCK_DIR:-/var/lib/eawr-build}
[ -w "$lock_dir/build.lock" ] || lock_dir=$base
# shellcheck source=../offload/linux_build_slots.sh
source "$helper"
job="shader-cross-p2-$$"
slot_acquire "$lock_dir" "$job" 14400 shader-cross-p2 "$queue_helper" 3 new
slot_label "dependency $job since $(date -u +%Y-%m-%dT%H:%M:%SZ) (pid $$)"
trap ': > "$slot_owner_file" 2>/dev/null' EXIT
mkdir -p "$output/source" "$output/build"
tar -xf "$archive" -C "$output/source" --strip-components=1
export SOURCE_DATE_EPOCH=0
{
    x86_64-w64-mingw32-g++-win32 -v
    x86_64-w64-mingw32-ld --version
    cmake --version
    ninja --version
    dpkg-query -W gcc-mingw-w64-x86-64-win32 g++-mingw-w64-x86-64-win32 binutils-mingw-w64-x86-64 mingw-w64-x86-64-dev
} > "$output/toolchain.txt" 2>&1
nice -n 10 cmake -S "$output/source" -B "$output/build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc-win32 \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++-win32 \
    "-DCMAKE_CXX_FLAGS=-ffile-prefix-map=$output=." \
    '-DCMAKE_SHARED_LINKER_FLAGS=-static-libgcc -static-libstdc++' \
    -DSPIRV_CROSS_SHARED=ON -DSPIRV_CROSS_STATIC=OFF -DSPIRV_CROSS_CLI=OFF \
    -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_GLSL=ON \
    -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_MSL=OFF \
    -DSPIRV_CROSS_ENABLE_CPP=OFF -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF \
    > "$output/configure.log" 2>&1
nice -n 10 cmake --build "$output/build" --target spirv-cross-c-shared -j 2 > "$output/build.log" 2>&1
x86_64-w64-mingw32-objdump -p "$output/build/libspirv-cross-c-shared.dll" > "$output/imports.txt"
sha256sum "$output/build/libspirv-cross-c-shared.dll"
