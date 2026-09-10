#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PACKAGE_DIR="../maplibre-ios-local"

usage() {
  cat <<'EOF'
Build MapLibre Native as an XCFramework and install it into local Swift package.

Paths resolve relative to MapLibre Native repository root.

Usage: scripts/build-ios-local.sh [options]

Options:
  --package-dir PATH      Local MapLibre Swift package. Default: ../maplibre-ios-local
  -h, --help              Show this help.
EOF
}

while (($#)); do
  case "$1" in
    --package-dir) PACKAGE_DIR="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

resolve_path() {
  (cd "$ROOT_DIR" && cd "$1" && pwd)
}

PACKAGE_DIR="$(resolve_path "$PACKAGE_DIR")"

[[ -d "$PACKAGE_DIR" ]] || { echo "Local package directory not found: $PACKAGE_DIR" >&2; exit 1; }
[[ -f "$PACKAGE_DIR/Package.swift" ]] || { echo "Package.swift not found in: $PACKAGE_DIR" >&2; exit 1; }

cd "$ROOT_DIR"
bazel build --compilation_mode=opt --features=dead_strip,thin_lto --objc_enable_binary_stripping \
  --apple_generate_dsym --output_groups=+dsyms --//:renderer=metal \
  //platform/ios:MapLibre.dynamic --embed_label="maplibre_ios_$(cat platform/ios/VERSION)"

ARTIFACT="$(bazel info execution_root)/$(bazel cquery --output=files --compilation_mode=opt --//:renderer=metal //platform/ios:MapLibre.dynamic)"
[[ -f "$ARTIFACT" ]] || { echo "XCFramework archive not found: $ARTIFACT" >&2; exit 1; }

TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIR"' EXIT
unzip -q "$ARTIFACT" -d "$TEMP_DIR"
FRAMEWORK_DIR="$(find "$TEMP_DIR" -type d -name MapLibre.xcframework -print -quit)"
[[ -n "$FRAMEWORK_DIR" ]] || { echo "MapLibre.xcframework not found in: $ARTIFACT" >&2; exit 1; }

rm -rf "$PACKAGE_DIR/MapLibre.xcframework"
mv "$FRAMEWORK_DIR" "$PACKAGE_DIR/MapLibre.xcframework"
echo "Installed $PACKAGE_DIR/MapLibre.xcframework"

