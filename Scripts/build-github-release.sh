#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
run_verification=1
overwrite=0

usage()
{
    echo "usage: $0 [--skip-verify] [--overwrite]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-verify)
            run_verification=0
            ;;
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

if [[ $run_verification -eq 1 ]]; then
    "$repo_root/Scripts/verify-mvp.sh"
fi

# verify-mvp.sh intentionally finishes with a debug bundle, so the optimized
# bundle must always be rebuilt after the verification gate.
"$repo_root/Scripts/build-app-bundle.sh" release

if [[ $overwrite -eq 1 ]]; then
    "$repo_root/Scripts/package-app-dmg.sh" --overwrite
else
    "$repo_root/Scripts/package-app-dmg.sh"
fi
