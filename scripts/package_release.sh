#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "usage: package_release.sh BUILD_DIR [VERSION] [OUTPUT_DIR]" >&2
  exit 2
fi

build_dir=$1
version=${2:-v0.1.0}
output_dir=${3:-dist}
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

case "$(uname -s)" in
  Darwin) platform=macos ;;
  Linux) platform=linux ;;
  *) echo "unsupported release host: $(uname -s)" >&2; exit 1 ;;
esac

case "$(uname -m)" in
  arm64|aarch64) architecture=arm64 ;;
  x86_64|amd64) architecture=x86_64 ;;
  *) echo "unsupported release architecture: $(uname -m)" >&2; exit 1 ;;
esac

package="mica-server-${version}-${platform}-${architecture}"
temporary=$(mktemp -d "${TMPDIR:-/tmp}/mica-release.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
stage="$temporary/$package"

cmake --install "$build_dir" --prefix "$stage"
install -m 0644 "$project_root/README.md" "$stage/README.md"
install -m 0644 "$project_root/LICENSE" "$stage/LICENSE"

if [[ "$platform" == macos ]]; then
  if otool -L "$stage/bin/mica-server" | grep -qi lua; then
    echo "release binary still links an external Lua library" >&2
    exit 1
  fi
else
  if ldd "$stage/bin/mica-server" | grep -qi lua; then
    echo "release binary still links an external Lua library" >&2
    exit 1
  fi
fi

mkdir -p "$output_dir"
archive_name="$package.tar.gz"
archive="$output_dir/$archive_name"
tar -C "$temporary" -czf "$archive" "$package"
(
  cd "$output_dir"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$archive_name" > "$archive_name.sha256"
  else
    shasum -a 256 "$archive_name" > "$archive_name.sha256"
  fi
)

echo "$archive"
