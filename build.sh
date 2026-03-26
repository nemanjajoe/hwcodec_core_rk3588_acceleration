#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="build"
BUILD_TYPE="Release"
JOBS="$(nproc)"
DO_CLEAN=0
DO_INSTALL=0
VERBOSE=0

usage() {
  cat <<'EOF'
Usage: ./build.sh [options]

Options:
  -d, --build-dir <dir>      Build directory (default: build)
  -t, --build-type <type>    CMAKE_BUILD_TYPE (default: Release)
  -j, --jobs <n>             Parallel jobs for build (default: nproc)
  -c, --clean                Remove build directory before configure
  -i, --install              Run "cmake --install" after build
  -v, --verbose              Enable verbose build output
  -h, --help                 Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -d|--build-dir)
      BUILD_DIR="${2:-}"
      shift 2
      ;;
    -t|--build-type)
      BUILD_TYPE="${2:-}"
      shift 2
      ;;
    -j|--jobs)
      JOBS="${2:-}"
      shift 2
      ;;
    -c|--clean)
      DO_CLEAN=1
      shift
      ;;
    -i|--install)
      DO_INSTALL=1
      shift
      ;;
    -v|--verbose)
      VERBOSE=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ -z "${BUILD_DIR}" || -z "${BUILD_TYPE}" || -z "${JOBS}" ]]; then
  echo "Invalid empty argument." >&2
  usage
  exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found in PATH." >&2
  exit 1
fi

if [[ "${DO_CLEAN}" -eq 1 ]]; then
  echo "[build.sh] clean: ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

echo "[build.sh] configure: BUILD_DIR=${BUILD_DIR}, BUILD_TYPE=${BUILD_TYPE}"
cmake -S . -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

BUILD_CMD=(cmake --build "${BUILD_DIR}" -j "${JOBS}")
if [[ "${VERBOSE}" -eq 1 ]]; then
  BUILD_CMD+=(--verbose)
fi

echo "[build.sh] build: jobs=${JOBS}"
"${BUILD_CMD[@]}"

if [[ "${DO_INSTALL}" -eq 1 ]]; then
  echo "[build.sh] install"
  cmake --install "${BUILD_DIR}"
fi

echo "[build.sh] done"
