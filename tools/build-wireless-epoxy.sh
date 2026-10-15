#!/usr/bin/env bash
# Build the pinned official mGBA libepoxy fork in an isolated UCRT64 prefix.
# Its EPOXY_STATIC_LIB build-time switch installs the Windows TLS callback;
# defining that switch only in an application cannot initialize stock epoxy.
set -euo pipefail
export PATH="/ucrt64/bin:/usr/bin:$PATH"

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
deps="$root/build-compact-deps"
commit=8b7513e88c4f23b6b8d6757b306e431af79df1ff
archive="$deps/libepoxy-8b7513e88.tar.gz"
source="$deps/libepoxy-$commit"
build="$deps/epoxy-build"
prefix="$deps/epoxy-static"

if [[ ! -x /ucrt64/bin/gcc.exe || ! -x /ucrt64/bin/pkg-config.exe ]]; then
    printf '%s\n' 'This build requires the matching MSYS2 UCRT64 GCC toolchain.' >&2
    exit 1
fi
mkdir -p -- "$deps" "$build" "$prefix" "$deps/epoxy-temp"
export TMPDIR="$deps/epoxy-temp"
if [[ ! -f $archive ]]; then
    /usr/bin/curl --fail --location --retry 3 --output "$archive" \
        "https://codeload.github.com/mgba-emu/libepoxy/tar.gz/$commit"
fi
printf '%s  %s\n' 7a9ed133020ad77dc59d545d56b2d84f28c72de37403d784c4baa07e26c967c8 "$archive" | sha256sum --check --status
if [[ ! -d $source ]]; then tar -xf "$archive" -C "$deps"; fi

cd -- "$build"
CC=/ucrt64/bin/gcc AR=/ucrt64/bin/ar RANLIB=/ucrt64/bin/ranlib \
    NM=/ucrt64/bin/nm STRIP=/ucrt64/bin/strip \
    CFLAGS='-O2 -DNDEBUG -DEPOXY_STATIC_LIB=ON' \
    "$source/configure" --enable-static --disable-shared \
        --host=x86_64-w64-mingw32 --prefix="$prefix"
make -j"${JOBS:-4}"
make install
mkdir -p -- "$prefix/share/licenses/libepoxy"
cp -- "$source/COPYING" "$prefix/share/licenses/libepoxy/COPYING"

# A stock Windows static archive may link successfully but lack initialization.
# Require the official fork's actual TLS-callback symbol in the library.
symbols=$(/ucrt64/bin/nm "$prefix/lib/libepoxy.a")
[[ $symbols == *' D dllmain_callback'* ]]
printf '%s\n' "Built official mGBA static libepoxy $commit in $prefix"
