#!/usr/bin/env bash
# リリースの tar.gz を作る。Steam Frame の上で実行する（本体の cairo・libnl・SteamVR の OpenVR にリンクするため）。
#   scripts/package.sh   → dist/frame-perf-overlay-<バージョン>.tar.gz
# 中身: 実行ファイル、install.sh、contrib（unit・.desktop・アイコン・設定の例）、LICENSE など。
set -euo pipefail
cd "$(dirname "$0")/.."

name="frame-perf-overlay"
build_dir="build-release"

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "Run this on the Steam Frame (aarch64). This machine is $(uname -m)." >&2
    exit 1
fi

# 開発用の build/ とは別のフォルダで、Release でビルドし直す
cmake -G Ninja -S . -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --clean-first

version="$("$build_dir/$name" --version | awk '{print $2}')"
if [[ -z "$version" || "$version" == "unknown" ]]; then
    echo "Could not read the version from $build_dir/$name --version" >&2
    exit 1
fi

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
root="$stage/$name"
mkdir -p "$root/contrib/icons"
install -m755 "$build_dir/$name" "$root/$name"
strip "$root/$name"
install -m755 install.sh "$root/"
install -m644 LICENSE THIRD_PARTY_LICENSES.md README.md README.ja.md CHANGELOG.md "$root/"
install -m644 "contrib/$name.service" "contrib/$name.desktop" contrib/config.example.json "$root/contrib/"
install -m644 contrib/icons/*.png "$root/contrib/icons/"
install -Dm644 licenses/LGPL-2.1.txt "$root/licenses/LGPL-2.1.txt"

mkdir -p dist
out="dist/$name-$version.tar.gz"
# 持ち主の名前は入れない（uid 0 にそろえる）
tar -C "$stage" --owner=0 --group=0 --numeric-owner --sort=name -czf "$out" "$name"
echo
tar -tzvf "$out"
echo
sha256sum "$out"
