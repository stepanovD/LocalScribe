#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app_bundle="$repo_root/Build/LocalScribe.app"
dist_dir="$repo_root/dist"
overwrite=0

usage()
{
    echo "usage: $0 [--overwrite]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --overwrite)
            overwrite=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
    shift
done

[[ -d "$app_bundle" ]] || {
    echo "application bundle is missing: $app_bundle" >&2
    echo "build it with Scripts/build-app-bundle.sh release" >&2
    exit 3
}

info_plist="$app_bundle/Contents/Info.plist"
executable="$app_bundle/Contents/MacOS/LocalScribeApp"
[[ -f "$info_plist" && -x "$executable" ]] || {
    echo "application bundle is incomplete: $app_bundle" >&2
    exit 4
}

version="$(plutil -extract CFBundleShortVersionString raw "$info_plist")"
[[ "$version" =~ ^[0-9A-Za-z][0-9A-Za-z._-]*$ ]] || {
    echo "unsupported release version: $version" >&2
    exit 5
}

architectures="$(lipo -archs "$executable")"
case "$architectures" in
    arm64)
        release_arch="arm64"
        ;;
    x86_64)
        release_arch="x86_64"
        ;;
    "arm64 x86_64"|"x86_64 arm64")
        release_arch="universal"
        ;;
    *)
        echo "unsupported application architectures: $architectures" >&2
        exit 6
        ;;
esac

codesign --verify --deep --strict --verbose=2 "$app_bundle"

dmg_name="LocalScribe-${version}-macOS-${release_arch}.dmg"
checksum_name="${dmg_name}.sha256"
dmg_path="$dist_dir/$dmg_name"
checksum_path="$dist_dir/$checksum_name"

if [[ $overwrite -eq 0 \
      && ( -e "$dmg_path" || -e "$checksum_path" ) ]]; then
    echo "release artifact already exists: $dmg_path" >&2
    echo "increment Config/Info.plist or pass --overwrite" >&2
    exit 7
fi

release_stage="$(mktemp -d /tmp/localscribe-dmg.XXXXXX)"
cleanup()
{
    case "$release_stage" in
        /tmp/localscribe-dmg.*)
            rm -rf "$release_stage"
            ;;
    esac
}
trap cleanup EXIT

ditto "$app_bundle" "$release_stage/LocalScribe.app"
ln -s /Applications "$release_stage/Applications"
mkdir -p "$dist_dir"

hdiutil_args=(
    create
    -volname "LocalScribe $version"
    -srcfolder "$release_stage"
    -format UDZO
)
if [[ $overwrite -eq 1 ]]; then
    hdiutil_args+=( -ov )
fi
hdiutil "${hdiutil_args[@]}" "$dmg_path"
hdiutil verify "$dmg_path"

(
    cd "$dist_dir"
    shasum -a 256 "$dmg_name" | tee "$checksum_name"
    shasum -a 256 -c "$checksum_name"
)

echo "$dmg_path"
echo "$checksum_path"
