#!/bin/sh
set -eu

asterisk_tree=${1:?usage: build-chan-mobile-v17-production.sh /path/to/patched/configured/asterisk-tree}
deps_root=${CM_LE_DEPS_ROOT:?set CM_LE_DEPS_ROOT to the extracted build dependency root}
lc3_root=${CM_LE_LC3_ROOT:?set CM_LE_LC3_ROOT to the pinned static liblc3 prefix}
expected_commit=da123773c723ed1263ff74569544f7ee84626c1a
expected_buildopt=1fb7f5c06d7a2052e38d021b3d8ca151
module=$asterisk_tree/addons/chan_mobile.so
sbc_archive=$deps_root/usr/lib/x86_64-linux-gnu/libsbc.a
lc3_archive=$lc3_root/lib/liblc3.a
canonical_source=/usr/src/asterisk-22.9.0

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	exit 1
}

[ -d "$asterisk_tree/.git" ] || fail "Asterisk tree is not a Git checkout"
[ "$(git -C "$asterisk_tree" rev-parse HEAD)" = "$expected_commit" ] ||
	fail "Asterisk HEAD is not the pinned 22.9.0 commit"
grep -Fq 'MBL_CALL_CONTROL_LE_GTBS' "$asterisk_tree/addons/chan_mobile.c" ||
	fail "v17 GTBS integration is missing"
grep -Fq 'LE GTBS readiness=ready' "$asterisk_tree/addons/chan_mobile.c" ||
	fail "v17 GTBS readiness marker is missing"
grep -Fq 'ast_mutex_trylock(&pvt->lock)' "$asterisk_tree/addons/chan_mobile.c" ||
	fail "v17 nonblocking mobile status path is missing"
grep -Fq 'cm_le_call_receive' "$asterisk_tree/addons/chan_mobile_lecall.c" ||
	fail "LE call-control helper is missing"
grep -Fq '#define CM_LE_HANDOFF_VERSION 3' \
	"$asterisk_tree/addons/chan_mobile_leaudio.h" ||
	fail "LE Audio v3 handoff contract is missing"
grep -Fq '#define CM_LE_LIFECYCLE_NORMAL_END 2' \
	"$asterisk_tree/addons/chan_mobile_leaudio.h" ||
	fail "normal-end lifecycle marker is missing"
grep -Fq 'cm_leaudio_normal_end(&pvt->leaudio)' \
	"$asterisk_tree/addons/chan_mobile.c" ||
	fail "normal-end channel handoff is missing"
[ -r "$asterisk_tree/addons/chan_mobile_msbc.c" ] ||
	fail "v10 mSBC helper is missing"
[ -r "$deps_root/usr/include/bluetooth/bluetooth.h" ] ||
	fail "staged BlueZ development headers are missing"
[ -r "$deps_root/usr/include/sbc/sbc.h" ] ||
	fail "staged SBC development headers are missing"
[ -r "$sbc_archive" ] || fail "staged static libsbc archive is missing"
[ -r "$lc3_root/include/lc3.h" ] || fail "pinned liblc3 header is missing"
[ -r "$lc3_archive" ] || fail "pinned static liblc3 archive is missing"
[ -x "$asterisk_tree/build_tools/make_buildopts_h" ] ||
	fail "Asterisk tree has not been configured"
[ -r "$asterisk_tree/menuselect.makeopts" ] ||
	fail "Asterisk menuselect.makeopts is missing"
# MENUSELECT_ADDONS lists the DISABLED modules.  Strip the assignment first:
# matching the raw line missed a first entry, because "=chan_mobile" is neither
# at the start of the line nor preceded by whitespace, and the build then
# reached the compiler with the module switched off.
if sed -n 's/^MENUSELECT_ADDONS=//p' "$asterisk_tree/menuselect.makeopts" |
	tr ' ' '\n' | grep -qx 'chan_mobile'; then
	fail "chan_mobile is disabled in menuselect.makeopts"
fi

makeopts_tmp=$(mktemp "$asterisk_tree/menuselect.makeopts.leaudio.XXXXXX")
buildopts_tmp=$(mktemp "$asterisk_tree/include/asterisk/buildopts.h.leaudio.XXXXXX")
trap 'rm -f -- "$makeopts_tmp" "$buildopts_tmp"' EXIT HUP INT TERM

grep -q '^MENUSELECT_CFLAGS=' "$asterisk_tree/menuselect.makeopts" ||
	fail "MENUSELECT_CFLAGS is missing"
sed 's/^MENUSELECT_CFLAGS=.*/MENUSELECT_CFLAGS=OPTIONAL_API TEST_FRAMEWORK /' \
	"$asterisk_tree/menuselect.makeopts" > "$makeopts_tmp"
mv "$makeopts_tmp" "$asterisk_tree/menuselect.makeopts"
(
	cd "$asterisk_tree"
	./build_tools/make_buildopts_h
) > "$buildopts_tmp"
mv "$buildopts_tmp" "$asterisk_tree/include/asterisk/buildopts.h"

grep -Fq "AST_BUILDOPT_SUM \"$expected_buildopt\"" \
	"$asterisk_tree/include/asterisk/buildopts.h" ||
	fail "Asterisk build option ABI does not match production"

common_cflags="-I$asterisk_tree/include -I$deps_root/usr/include"
common_cflags="$common_cflags -O2 -g3 -pipe -Wall -Wstrict-prototypes"
common_cflags="$common_cflags -Wmissing-prototypes -Wmissing-declarations"
common_cflags="$common_cflags -I$lc3_root/include"
common_cflags="$common_cflags -ffile-prefix-map=$asterisk_tree=$canonical_source"
common_cflags="$common_cflags -fdebug-prefix-map=$asterisk_tree=$canonical_source"
# KNOWN LIMITATION: only the Asterisk tree is mapped.  $deps_root and $lc3_root
# leak into debug info, so this build is reproducible only when those two live
# at the same absolute paths as the original.  Adding
#   -ffile-prefix-map=$deps_root=/usr -ffile-prefix-map=$lc3_root=/liblc3
# makes it path independent, but it also changes the output hash, so it must
# land on a version bump rather than under an existing sealed hash.
common_ldflags="-L$deps_root/usr/lib/x86_64-linux-gnu"

env _ASTCFLAGS="$common_cflags" _ASTLDFLAGS="$common_ldflags" SOLINK=-shared \
	make -C "$asterisk_tree/addons" ASTTOPDIR="$asterisk_tree" clean-chan_mobile.c
rm -f -- "$asterisk_tree/addons/chan_mobile.o" "$module"

env LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=1775751866 \
	_ASTCFLAGS="$common_cflags" _ASTLDFLAGS="$common_ldflags" SOLINK=-shared \
	make -j2 -C "$asterisk_tree/addons" ASTTOPDIR="$asterisk_tree" \
		SBC_LIB="$sbc_archive" LC3_LIB="$lc3_archive" chan_mobile.so

[ -r "$module" ] || fail "build did not produce chan_mobile.so"
if readelf -d "$module" | grep -Eq 'RPATH|RUNPATH|libsbc|liblc3'; then
	fail "module has a runtime path or a dynamic SBC/LC3 dependency"
fi
if readelf -Ws "$module" | grep -Eq ' UND .*\b(lc3_|sbc_)'; then
	fail "module retains unresolved SBC or LC3 symbols"
fi
strings "$module" | grep -Fq "AST_BUILDOPT_SUM \"$expected_buildopt\"" ||
	fail "module does not carry the production Asterisk ABI checksum"

needed=$(readelf -d "$module" |
	sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' |
	LC_ALL=C sort | tr '\n' ' ')
[ "$needed" = "libbluetooth.so.3 libc.so.6 " ] ||
	fail "unexpected dynamic dependency set: $needed"

sha256sum "$module"
printf '%s\n' \
	'PASS: v17 production ABI, static SBC/LC3, lifecycle normal-end marker, bounded dependencies'
