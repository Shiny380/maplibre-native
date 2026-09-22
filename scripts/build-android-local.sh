#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="$ROOT_DIR/platform/android"

RENDERER="vulkan"
BUILD_TYPE="release"
ABIS="all"
MAVEN_REPO="${HOME}/.m2/repository"

usage() {
  cat <<'EOF'
Build MapLibre Native for Android and publish it to the local Maven repository.

Defaults:
  renderer:   vulkan
  build type: release
  ABIs:       all
  Maven repo: ~/.m2/repository

Usage: scripts/build-android-local.sh [options]

Options:
  --renderer NAME       Renderer: vulkan, opengl, or multiBackend.
  --build-type TYPE     Build type: release or debug.
  --abis LIST           ABI selection passed to maplibre.abis.
                        Use "all" or a comma/space-separated subset of:
                        armeabi-v7a, arm64-v8a, x86, x86_64.
  --maven-repo PATH     Local Maven repository. Default: ~/.m2/repository
  -h, --help            Show this help.
EOF
}

require_value() {
  if (($# < 2)); then
    echo "Missing value for $1" >&2
    usage >&2
    exit 2
  fi
}

while (($#)); do
  case "$1" in
    --renderer)
      require_value "$@"
      RENDERER="$2"
      shift 2
      ;;
    --build-type)
      require_value "$@"
      BUILD_TYPE="$2"
      shift 2
      ;;
    --abis)
      require_value "$@"
      ABIS="$2"
      shift 2
      ;;
    --maven-repo)
      require_value "$@"
      MAVEN_REPO="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

RENDERER_LOWER="$(printf '%s' "$RENDERER" | tr '[:upper:]' '[:lower:]')"
case "$RENDERER_LOWER" in
  vulkan)
    RENDERER="vulkan"
    PUBLICATION_PREFIX="Vulkan"
    ARTIFACT_ID="android-sdk-vulkan"
    ;;
  opengl)
    RENDERER="opengl"
    PUBLICATION_PREFIX="Opengl"
    ARTIFACT_ID="android-sdk-opengl"
    ;;
  multibackend|multi-backend)
    RENDERER="multiBackend"
    PUBLICATION_PREFIX="Multibackend"
    ARTIFACT_ID="android-sdk-vulkan-opengl"
    ;;
  *)
    echo "Unsupported renderer: $RENDERER" >&2
    exit 2
    ;;
esac

BUILD_TYPE_LOWER="$(printf '%s' "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
case "$BUILD_TYPE_LOWER" in
  release)
    BUILD_TYPE="release"
    ;;
  debug)
    BUILD_TYPE="debug"
    ARTIFACT_ID="$ARTIFACT_ID-debug"
    ;;
  *)
    echo "Unsupported build type: $BUILD_TYPE" >&2
    exit 2
    ;;
esac

ABIS="${ABIS//,/ }"
read -r -a ABI_VALUES <<< "$ABIS"
if (("${#ABI_VALUES[@]}" == 0)); then
  echo "At least one ABI is required" >&2
  exit 2
fi

if [[ "${ABI_VALUES[0]}" == "all" ]]; then
  if (("${#ABI_VALUES[@]}" != 1)); then
    echo "'all' cannot be combined with individual ABIs" >&2
    exit 2
  fi
  ABIS="all"
else
  for abi in "${ABI_VALUES[@]}"; do
    case "$abi" in
      armeabi-v7a|arm64-v8a|x86|x86_64) ;;
      *)
        echo "Unsupported ABI: $abi" >&2
        exit 2
        ;;
    esac
  done
  ABIS="${ABI_VALUES[*]}"
fi

command -v java >/dev/null 2>&1 || {
  echo "java not found in PATH; Java 17 is required" >&2
  exit 1
}

[[ -x "$ANDROID_DIR/gradlew" ]] || {
  echo "Gradle wrapper not found or not executable: $ANDROID_DIR/gradlew" >&2
  exit 1
}

VERSION_FILE="$ANDROID_DIR/VERSION"
[[ -f "$VERSION_FILE" ]] || {
  echo "Android VERSION file not found: $VERSION_FILE" >&2
  exit 1
}
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"

MAVEN_REPO="$(mkdir -p "$MAVEN_REPO" && cd "$MAVEN_REPO" && pwd)"
UNUSED_REPOSITORY="$(mktemp -d)"
trap 'rm -rf "$UNUSED_REPOSITORY"' EXIT

PUBLICATION="${PUBLICATION_PREFIX}${BUILD_TYPE}"
TASK=":MapLibreAndroid:publish${PUBLICATION}PublicationToMavenLocal"

echo "Building MapLibre Android"
echo "  renderer:   $RENDERER"
echo "  build type: $BUILD_TYPE"
echo "  ABIs:       $ABIS"
echo "  version:    $VERSION"
echo "  artifact:   org.maplibre.gl:$ARTIFACT_ID:$VERSION"
echo "  Maven repo: $MAVEN_REPO"

cd "$ANDROID_DIR"

# Supplying a configured repository prevents the normal Maven Central/signing
# setup from being enabled. The selected task still publishes only to MavenLocal.
./gradlew --parallel \
  "-Pmaplibre.abis=$ABIS" \
  "-PpublicationRepositoryUrl=file://$UNUSED_REPOSITORY" \
  "-Dmaven.repo.local=$MAVEN_REPO" \
  "$TASK"

ARTIFACT_DIR="$MAVEN_REPO/org/maplibre/gl/$ARTIFACT_ID/$VERSION"
AAR="$ARTIFACT_DIR/$ARTIFACT_ID-$VERSION.aar"
POM="$ARTIFACT_DIR/$ARTIFACT_ID-$VERSION.pom"

[[ -f "$AAR" ]] || {
  echo "Published AAR not found: $AAR" >&2
  exit 1
}
[[ -f "$POM" ]] || {
  echo "Published POM not found: $POM" >&2
  exit 1
}

echo
echo "Published successfully:"
echo "  $AAR"
echo
echo "Gradle dependency:"
echo "  implementation(\"org.maplibre.gl:$ARTIFACT_ID:$VERSION\")"
echo
echo "Ensure mavenLocal() is listed before mavenCentral() in the consuming project."
echo "If rebuilding the same version, use --refresh-dependencies in the consuming project if needed."
