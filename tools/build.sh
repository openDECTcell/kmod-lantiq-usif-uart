#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the kmod package with the OpenWrt SDK and stage it in deploy/,
# together with the usif-loop test tool.
#
# Usage: tools/build.sh [path-to-sdk]
# Default SDK: build/openwrt-sdk-*lantiq-xrx200* in this repository
# (gitignored). The SDK builds from its own copy of Makefile and src/, so
# this tree stays free of build output.
#
# The SDK's host check insists on the ncurses headers, which only
# menuconfig needs. Without them: run the check once
#   make -C <sdk> -r -s -f include/prereq-build.mk prereq TOPDIR=<sdk>
# and, if ncurses.h is the only failure, touch
# <sdk>/staging_dir/host/.prereq-build.
set -e

here=$(cd "$(dirname "$0")/.." && pwd)

sdk=${1:-$(ls -d "$here"/build/openwrt-sdk-*lantiq-xrx200* 2>/dev/null | head -1)}
if [ -z "$sdk" ] || [ ! -d "$sdk" ]; then
	echo "no SDK found; pass the path as an argument" >&2
	exit 1
fi
sdk=$(cd "$sdk" && pwd)

pkg=kmod-lantiq-usif-uart
pkgdir="$sdk/package/$pkg"
rm -rf "$pkgdir"
mkdir -p "$pkgdir"
cp -a "$here/Makefile" "$pkgdir/"
cp -a "$here/src" "$pkgdir/"

(
	cd "$sdk"
	# a fresh SDK has no .config and would start menuconfig
	[ -f .config ] || make defconfig >/dev/null
	make package/$pkg/clean >/dev/null 2>&1 || true
	make package/$pkg/compile V=s
)

kver=$(ls "$sdk"/build_dir/target-*/linux-lantiq_xrx200/ | sed -n 's/^linux-//p' | head -1)
ko=$(ls "$sdk"/build_dir/target-*/linux-lantiq_xrx200/lantiq-usif-uart-*/.pkgdir/$pkg/lib/modules/"$kver"/lantiq_usif_uart.ko 2>/dev/null | head -1)
apk=$(ls "$sdk"/bin/targets/lantiq/xrx200/packages/$pkg-*.apk 2>/dev/null | head -1)

mkdir -p "$here/deploy"
cc=$(ls "$sdk"/staging_dir/toolchain-*/bin/*-openwrt-linux-musl-gcc | head -1)
STAGING_DIR="$sdk/staging_dir" "$cc" -O2 -Wall -o "$here/deploy/usif-loop" \
	"$here/tools/usif-loop.c"
[ -n "$ko" ] && cp -a "$ko" "$here/deploy/"
[ -n "$apk" ] && cp -a "$apk" "$here/deploy/"

echo "artifacts in $here/deploy:"
ls -l "$here/deploy"
