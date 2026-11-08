#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Host unit test, host compile of tools/usif-loop.c, W=1 out-of-tree
# kbuild and checkpatch, without touching the source tree (the kbuild runs
# in a temporary copy).
#
# Usage: tools/check.sh [path-to-sdk]
set -e

here=$(cd "$(dirname "$0")/.." && pwd)

sdk=${1:-$(ls -d "$here"/build/openwrt-sdk-*lantiq-xrx200* 2>/dev/null | head -1)}
if [ -z "$sdk" ] || [ ! -d "$sdk" ]; then
	echo "no SDK found; pass the path as an argument" >&2
	exit 1
fi
sdk=$(cd "$sdk" && pwd)
kdir=$(ls -d "$sdk"/build_dir/target-*/linux-lantiq_xrx200/linux-* | head -1)
target=$(ls -d "$sdk"/staging_dir/target-* | head -1)
toolbin=$(ls -d "$sdk"/staging_dir/toolchain-*/bin | head -1)

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "== host unit test"
cp -a "$here/src" "$here/tests" "$tmp/"
make -s -C "$tmp/tests"

echo "== usif-loop (host compile only)"
${CC:-cc} -O2 -Wall -Wextra -Werror -o "$tmp/usif-loop" "$here/tools/usif-loop.c"

echo "== kbuild W=1 ($(basename "$kdir"))"
mkdir "$tmp/kb"
cp "$here"/src/*.c "$here"/src/*.h "$here"/src/Makefile "$tmp/kb/"
# STAGING_DIR must name the target directory, or W=1 trips over a
# missing include directory.
PATH="$toolbin:$PATH" STAGING_DIR="$target" \
	make -s -C "$kdir" M="$tmp/kb" ARCH=mips \
	CROSS_COMPILE=mips-openwrt-linux-musl- W=1 modules
ls -l "$tmp/kb/lantiq_usif_uart.ko"

echo "== checkpatch"
perl "$kdir/scripts/checkpatch.pl" --no-tree --terse -f \
	"$here"/src/*.c "$here"/src/*.h
