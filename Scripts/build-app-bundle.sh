#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
configuration="${1:-release}"
case "$configuration" in
    debug|release) ;;
    *)
        echo "usage: $0 [debug|release]" >&2
        exit 2
        ;;
esac

if [[ -n "${LOCALSCRIBE_SDKROOT:-}" ]]; then
    task_sdkroot="$LOCALSCRIBE_SDKROOT"
elif [[ -d /Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk ]]; then
    task_sdkroot="/Library/Developer/CommandLineTools/SDKs/MacOSX15.4.sdk"
else
    task_sdkroot="$(xcrun --sdk macosx --show-sdk-path)"
fi

module_cache="$repo_root/.build/localscribe-module-cache"
mkdir -p "$module_cache"

env \
    SDKROOT="$task_sdkroot" \
    CLANG_MODULE_CACHE_PATH="$module_cache/clang" \
    SWIFTPM_MODULECACHE_OVERRIDE="$module_cache/swiftpm" \
    swift build \
        --package-path "$repo_root" \
        --configuration "$configuration" \
        --disable-sandbox

bin_dir="$(
    env SDKROOT="$task_sdkroot" swift build \
        --package-path "$repo_root" \
        --configuration "$configuration" \
        --show-bin-path
)"
executable="$bin_dir/LocalScribeApp"
metal_probe="$bin_dir/LocalScribeMetalProbe"
framework="$bin_dir/whisper.framework"
bundle="$repo_root/Build/LocalScribe.app"

[[ -x "$executable" ]] || {
    echo "LocalScribeApp executable was not produced" >&2
    exit 3
}
[[ -d "$framework" ]] || {
    echo "whisper.framework was not produced" >&2
    exit 4
}
[[ -x "$metal_probe" ]] || {
    echo "LocalScribeMetalProbe executable was not produced" >&2
    exit 5
}

rm -rf "$bundle"
mkdir -p \
    "$bundle/Contents/MacOS" \
    "$bundle/Contents/Frameworks" \
    "$bundle/Contents/Resources/Licenses"
cp "$repo_root/Config/Info.plist" "$bundle/Contents/Info.plist"
cp "$executable" "$bundle/Contents/MacOS/LocalScribeApp"
cp "$metal_probe" "$bundle/Contents/MacOS/LocalScribeMetalProbe"
cp "$repo_root/LICENSE.md" \
    "$bundle/Contents/Resources/LICENSE.md"
cp "$repo_root/NOTICE" \
    "$bundle/Contents/Resources/NOTICE"
cp "$repo_root/THIRD_PARTY_NOTICES.md" \
    "$bundle/Contents/Resources/THIRD_PARTY_NOTICES.md"
cp "$repo_root/Vendor/whisper-LICENSE" \
    "$bundle/Contents/Resources/Licenses/whisper-LICENSE"
mkdir -p "$bundle/Contents/Frameworks/whisper.framework"
rsync \
    -a \
    --exclude=.DS_Store \
    "$framework/" \
    "$bundle/Contents/Frameworks/whisper.framework/"

bundled_whisper="$bundle/Contents/Frameworks/whisper.framework/whisper"
[[ -f "$bundled_whisper" ]] || {
    echo "bundled whisper framework binary is missing" >&2
    exit 5
}
otool -L "$bundled_whisper" \
    | grep -q '/Metal.framework/'
strings -a "$bundled_whisper" \
    | grep 'using embedded metal library' >/dev/null

codesign \
    --force \
    --sign - \
    "$bundle/Contents/Frameworks/whisper.framework"
codesign \
    --force \
    --sign - \
    "$bundle/Contents/MacOS/LocalScribeMetalProbe"
codesign \
    --force \
    --sign - \
    --entitlements "$repo_root/Config/LocalScribe.entitlements" \
    "$bundle"

plutil -lint "$bundle/Contents/Info.plist"
codesign --verify --deep --strict "$bundle"
otool -L "$bundle/Contents/MacOS/LocalScribeApp" \
    | grep -q '@rpath/whisper.framework/'

echo "$bundle"
