#!/usr/bin/env bash
# Isolated UCRT64 FFmpeg dependency build. No application source or installed
# MSYS2 package is changed. Run with the MSYS2 bash belonging to /ucrt64.
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
deps="$root/build-compact-deps"
prefix="$deps/ffmpeg-static"
source_commit=239f2c733de417201d7ad3b3b8b0d9b63285b2b1
nv_commit=c69278340ab1d5559c7d7bf0edf615dc33ddbba7
recipe_commit=dad3d8a2bbc452b58139ebf21bc2ab9d81b9c4e9
source="$deps/FFmpeg-$source_commit"
nv_source="$deps/nv-codec-headers-$nv_commit"
build="$deps/ffmpeg-build"
recipe="$deps/configure-ffmpeg-official.sh"
export PATH="$deps/nasm-tool/ucrt64/bin:/ucrt64/bin:/usr/bin:$PATH"

if [[ ! -x /ucrt64/bin/gcc.exe || ! -x /ucrt64/bin/pkg-config.exe ]]; then
    printf '%s\n' 'This build requires the matching MSYS2 UCRT64 GCC toolchain.' >&2
    exit 1
fi
mkdir -p -- "$deps" "$deps/ffmpeg-temp"
export TMPDIR="$deps/ffmpeg-temp"

download() {
    local path=$1 url=$2 sha=$3
    if [[ ! -f $path ]]; then
        /usr/bin/curl --fail --location --retry 3 --output "$path" "$url"
    fi
    printf '%s  %s\n' "$sha" "$path" | sha256sum --check --status
}

download "$deps/ffmpeg-source-239f2c733.tar.gz" \
    "https://codeload.github.com/FFmpeg/FFmpeg/tar.gz/$source_commit" \
    ec0aa20fb9f6fd3692ffc04dc12ffa43cfffc4a479e388ccd7910ec6cfe188a2
download "$deps/nv-codec-headers-c69278340.tar.gz" \
    "https://codeload.github.com/FFmpeg/nv-codec-headers/tar.gz/$nv_commit" \
    0d06ccc7410671e8ea075f29abb7c1f6c793574d8c768dc1bc96b142797b6628
download "$recipe" \
    "https://raw.githubusercontent.com/mgba-emu/dependencies/$recipe_commit/libraries/buildscripts/configure-ffmpeg.sh" \
    2071997846c7ce75ca407756a0fee88f1481eef15798b47f37dca1dfd8ae4e16
# This package's checksum matches the MSYS2 repository package database.
download "$deps/nasm-3.01-1.pkg.tar.zst" \
    'https://repo.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-nasm-3.01-1-any.pkg.tar.zst' \
    80dfd60dca67c91bd72c269b0d6b4b581130471bea0f3268e8cd57e4bb87b1ba

if [[ ! -d $source ]]; then tar -xf "$deps/ffmpeg-source-239f2c733.tar.gz" -C "$deps"; fi
if [[ ! -d $nv_source ]]; then tar -xf "$deps/nv-codec-headers-c69278340.tar.gz" -C "$deps"; fi
if [[ ! -x $deps/nasm-tool/ucrt64/bin/nasm.exe ]]; then
    mkdir -p -- "$deps/nasm-tool"
    tar -xf "$deps/nasm-3.01-1.pkg.tar.zst" -C "$deps/nasm-tool"
fi

make -C "$nv_source" PREFIX="$prefix" install
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:/ucrt64/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig:/ucrt64/lib/pkgconfig:/ucrt64/share/pkgconfig"

# Take the literal component switches from mGBA's pinned dependency recipe.
# Its toolchain detection is replaced by explicit UCRT64 settings below. The
# Windows NVENC switches are added explicitly because the original has them
# in a shell variable before the configure command.
options=()
in_command=0
while IFS= read -r line; do
    if [[ $line == 'set -x' ]]; then in_command=1; continue; fi
    if (( ! in_command )); then continue; fi
    for token in $line; do
        case "$token" in
            --enable-*|--disable-*) options+=("$token") ;;
        esac
    done
done < "$recipe"

mkdir -p -- "$build" "$prefix"
cd -- "$build"
"$source/configure" "${options[@]}" \
    --disable-autodetect --enable-zlib \
    --enable-ffnvcodec --enable-nvenc --enable-encoder=h264_nvenc --enable-encoder=hevc_nvenc \
    --arch=x86_64 --target-os=mingw32 \
    --cc=/ucrt64/bin/gcc --cxx=/ucrt64/bin/g++ --ld=/ucrt64/bin/g++ \
    --prefix="$prefix" --pkg-config=/ucrt64/bin/pkg-config \
    --pkg-config-flags=--static --disable-debug \
    --extra-cflags="-I$prefix/include" \
    --extra-ldflags="-L$prefix/lib -static-libgcc -static-libstdc++"
# A GitHub source archive inside another Git checkout must not inherit that
# parent project's git-describe version. Pass the pinned revision explicitly.
revision="8.1.1-${source_commit:0:9}" "$source/ffbuild/version.sh" "$source" libavutil/ffversion.h
make -j"${JOBS:-4}" REVISION="8.1.1-${source_commit:0:9}"
make install REVISION="8.1.1-${source_commit:0:9}"
mkdir -p -- "$prefix/share/licenses/ffmpeg"
cp -- "$source/COPYING.GPLv2" "$source/COPYING.GPLv3" "$source/LICENSE.md" \
    "$prefix/share/licenses/ffmpeg/"
printf 'FFmpeg static prefix: %s\n' "$prefix"
printf 'FFmpeg source commit: %s\n' "$source_commit"
printf 'mGBA dependency recipe commit: %s\n' "$recipe_commit"
