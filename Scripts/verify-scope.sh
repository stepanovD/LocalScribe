#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

failed=0

if rg -n \
    --glob 'Package.swift' \
    --glob 'CMakeLists.txt' \
    --glob '*.pbxproj' \
    '(iOS|iPadOS|watchOS|tvOS|visionOS|Mac Catalyst|macCatalyst|iphoneos|iphonesimulator)'
then
    echo "scope check: a non-macOS product target was declared" >&2
    failed=1
fi

if rg -n \
    --glob '*.{swift,h,hpp,c,cc,cpp}' \
    '(URLSession|NSURLSession|NWConnection|CFNetwork|Network\.framework|https?://)' \
    Core macOS
then
    echo "scope check: a network API or endpoint entered the product sources" >&2
    failed=1
fi

if rg -n \
    --glob '*.{h,hpp,c,cc,cpp}' \
    '(SwiftUI|AppKit|ScreenCaptureKit|AVFoundation|Security\.framework|Keychain)' \
    Core
then
    echo "scope check: an Apple platform type entered the portable core" >&2
    failed=1
fi

if rg -n \
    --glob '*.{swift,h,hpp,c,cc,cpp}' \
    '(OpenAI|Claude|Anthropic|calendar|cloud sync|summari[sz])' \
    Core macOS
then
    echo "scope check: an explicitly excluded product capability entered sources" >&2
    failed=1
fi

if [[ "$failed" -ne 0 ]]; then
    exit 1
fi

echo "scope check: macOS-only, offline product boundary verified"
