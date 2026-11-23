#!/usr/bin/env bash
set -euo pipefail

work="$RUNNER_TEMP/neutrino-release"
dist="$RUNNER_TEMP/neutrino-dist"
package="$work/package"
mkdir -p "$package/licenses" "$dist"
args=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCURL_NO_CURL_CMAKE=ON)

if [[ "$RUNNER_OS" == Linux ]]; then
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends clang-20 libcurl4-openssl-dev
    export CC=clang-20
    multiarch="$(gcc -print-multiarch)"
    args+=("-DCURL_INCLUDE_DIR=/usr/include/$multiarch"
           "-DCURL_LIBRARY=/usr/lib/$multiarch/libcurl.so")
    runtime='Built on Ubuntu 24.04. Requires glibc and libcurl.so.4.'
else
    CC="$(xcrun --find clang)"
    if ! printf '%s\n' 'int main(void) { void *p = nullptr; return p != nullptr; }' |
        "$CC" -std=c23 -x c -fsyntax-only -; then
        HOMEBREW_NO_AUTO_UPDATE=1 brew install llvm@21
        CC="$(brew --prefix llvm@21)/bin/clang"
    fi
    export CC
    sdk="$(xcrun --sdk macosx --show-sdk-path)"
    args+=("-DCMAKE_OSX_SYSROOT=$sdk" -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0
           -DCMAKE_C_FLAGS=-D_DARWIN_C_SOURCE "-DCURL_INCLUDE_DIR=$sdk/usr/include"
           "-DCURL_LIBRARY=$sdk/usr/lib/libcurl.tbd")
    runtime='Requires macOS 15 or later. Uses system libcurl.'
fi

cmake -S . -B "$work/build" "${args[@]}"
cmake --build "$work/build" --parallel 3
ctest --test-dir "$work/build" --output-on-failure --no-tests=error
install -m 755 "$work/build/neutrino" "$package/neutrino"
cp LICENSE "$package/LICENSE"
cp third_party/cjson/LICENSE "$package/licenses/cJSON.txt"

"$package/neutrino" --demo --snapshot > "$work/demo.txt"
grep -qi neutrino "$work/demo.txt"
{
    printf 'Source %s\nPlatform %s\n%s\n' "$(git rev-parse HEAD)" "$PACKAGE_PLATFORM" "$runtime"
    "$package/neutrino" --version
} > "$package/build-info.txt"

if [[ "$RUNNER_OS" == Linux ]]; then
    ldd "$package/neutrino" | tee "$package/dependencies.txt"
    if grep -q 'not found' "$package/dependencies.txt"; then exit 1; fi
else
    otool -L "$package/neutrino" | tee "$package/dependencies.txt"
    awk 'NR > 1 && $1 !~ /^\/usr\/lib\// && $1 !~ /^\/System\/Library\// { exit 1 }' "$package/dependencies.txt"
fi

tar -czf "$dist/neutrino-$PACKAGE_ID-$PACKAGE_PLATFORM.tar.gz" \
    -C "$package" LICENSE licenses neutrino build-info.txt dependencies.txt
