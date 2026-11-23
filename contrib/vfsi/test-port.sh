#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
binary="$repo/git"
library=${VFSI_LIBRARY:?set VFSI_LIBRARY to the VFSI adapter}
fixture=$(mktemp -d /tmp/vfsi-git-port.XXXXXX)
trap 'rm -rf "${fixture:?}"' EXIT HUP INT TERM
"$binary" -C "$fixture" init -q
"$binary" -C "$fixture" config user.email vfsi-test@example.invalid
"$binary" -C "$fixture" config user.name VFSI
for n in 1 2 3 4; do
    printf '%s\n' "$n" > "$fixture/file-$n"
done
"$binary" -C "$fixture" add .
"$binary" -C "$fixture" commit -qm fixture
"$binary" -C "$fixture" count-objects -v > "$fixture/expected"
for budget in 0 1 67108864; do
    VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	VFSI_PREFETCH_OBJECTS=1 VFSI_PREFETCH_BYTES=$budget \
	"$binary" -C "$fixture" count-objects -v > "$fixture/actual" 2>"$fixture/trace"
    grep -q "vfsi: listdirs completed" "$fixture/trace"
    grep -q 'phase=collect application_callbacks=0' "$fixture/trace"
    diff -u "$fixture/expected" "$fixture/actual"
    VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	VFSI_PREFETCH_OBJECTS=1 VFSI_PREFETCH_BYTES=$budget \
	"$binary" -C "$fixture" fsck --full
done
printf '%s\n' 'Git port parity passed (zero, tiny and normal prefetch budgets).'

# A nonexistent library must leave the native result intact.
VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/missing-vfsi.so" \
    VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
diff -u "$fixture/expected" "$fixture/actual"
grep -q 'vfsi: cannot load ' "$fixture/trace"
if grep -q 'vfsi: listdirs completed' "$fixture/trace"; then exit 1; fi

# A loaded-but-incompatible adapter must not silently masquerade as acceleration.
include=${VFSI_INCLUDE:-"$repo/build-vfsi-c/vfsi-c-0.3.25/sdk/include"}
${CC:-cc} -Wall -Wextra -Werror -I"$include" -I"$repo/contrib/vfsi" \
    "$repo/contrib/vfsi/test-identity.c" -o "$fixture/test-identity"
"$fixture/test-identity"
${CC:-cc} -shared -fPIC -I"$include" "$repo/contrib/vfsi/test-proxy.c" -ldl -o "$fixture/proxy.so"
export VFSI_TEST_REAL_LIBRARY="$library"
for fault in VFSI_TEST_BAD_ABI VFSI_TEST_PARTIAL VFSI_TEST_BAD_INDEX; do
    env "$fault=1" VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/proxy.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	"$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
    diff -u "$fixture/expected" "$fixture/actual"
    if grep -q 'vfsi: listdirs completed' "$fixture/trace"; then exit 1; fi
done
# A legacy callback stop still returns success from vfsi_listdir.
mkdir -p "$fixture/.git/objects/aa" "$fixture/.git/objects/bb"
VFSI_TEST_BAD_ROOT=1 VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/proxy.so" \
    VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
diff -u "$fixture/expected" "$fixture/actual"
if grep -q 'vfsi: listdirs completed' "$fixture/trace"; then exit 1; fi

# Native enumeration follows a symlink used for a loose-object fanout.
fanout=$(find "$fixture/.git/objects" -mindepth 1 -maxdepth 1 -type d -name '[0-9a-f][0-9a-f]' ! -empty | head -n 1)
test -n "$fanout"
mv "$fanout" "$fixture/fanout"
ln -s "$fixture/fanout" "$fanout"
for adapter in "$library" "$fixture/proxy.so"; do
    VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$adapter" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	"$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
    diff -u "$fixture/expected" "$fixture/actual"
    if grep -q 'vfsi: listdirs completed' "$fixture/trace"; then exit 1; fi
done
rm "$fanout"
mv "$fixture/fanout" "$fanout"
# Preserve the native spelling of a caller-supplied object directory.
object_dir="$fixture/.git/objects/"
printf 'loose garbage\n' > "${object_dir}aa/garbage"
GIT_OBJECT_DIRECTORY="$object_dir" VFSI_IMPL=off \
    "$binary" -C "$fixture" count-objects -v >"$fixture/native-slash" 2>"$fixture/native-warnings"
GIT_OBJECT_DIRECTORY="$object_dir" VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$library" \
    VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" count-objects -v >"$fixture/vector-slash" 2>"$fixture/trace"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
grep '^warning:' "$fixture/trace" >"$fixture/vector-warnings"
diff -u "$fixture/native-slash" "$fixture/vector-slash"
diff -u "$fixture/native-warnings" "$fixture/vector-warnings"
rm "${object_dir}aa/garbage"
# An invalid byte budget must never dispatch a payload read.
VFSI_TEST_CALL_LOG="$fixture/bad-budget" VFSI_TRACE=1 VFSI_IMPL=dummy \
    VFSI_LIBRARY="$fixture/proxy.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    VFSI_PREFETCH_OBJECTS=1 VFSI_PREFETCH_BYTES=' -1' \
    "$binary" -C "$fixture" fsck --full >"$fixture/actual" 2>"$fixture/trace"
grep -q 'invalid prefetch budget' "$fixture/trace"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
test ! -e "$fixture/bad-budget"

# Initialization errors happen before callbacks and must retain POSIX enumeration.
VFSI_IMPL=off "$binary" -C "$fixture" cat-file --batch-all-objects --batch-check >"$fixture/native-open-objects"
set -- dummy
if test "$(uname -s)" = Linux; then set -- "$@" nfs; fi
for mode do
	for command in count objects; do
		case "$command" in
			count) set -- count-objects -v; expected="$fixture/expected" ;;
			objects) set -- cat-file --batch-all-objects --batch-check; expected="$fixture/native-open-objects" ;;
		esac
		VFSI_TEST_OPEN_FAIL=1 VFSI_TRACE=1 VFSI_IMPL="$mode" VFSI_LIBRARY="$fixture/proxy.so" \
			VFSI_ROOT="$fixture" VFSI_HOST=127.0.0.1 VFSI_EXPORT=/ VFSI_MOUNT="$fixture" \
			"$binary" -C "$fixture" "$@" >"$fixture/actual" 2>"$fixture/trace"
		diff -u "$expected" "$fixture/actual"
		grep -q 'vfsi: open failed: 5' "$fixture/trace"
		if grep -q 'vfsi: listdirs completed' "$fixture/trace"; then exit 1; fi
	done
done
printf '%s\n' 'Filesystem initialization failure retains POSIX object enumeration.'
if test "$(uname -s)" = Linux; then
	cat >"$fixture/mount-table.c" <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

FILE *fopen(const char *path, const char *mode)
{
	FILE *(*open_file)(const char *, const char *) = dlsym(RTLD_NEXT, "fopen");
	if (!strcmp(path, "/proc/self/mountinfo"))
		path = getenv("VFSI_TEST_MOUNTINFO");
	return open_file(path, mode);
}

FILE *fopen64(const char *path, const char *mode)
{
	return fopen(path, mode);
}
EOF
	${CC:-cc} -shared -fPIC "$fixture/mount-table.c" -ldl -o "$fixture/mount-table.so"
	printf '%s\n' "1 0 0:1 / / rw - tmpfs tmpfs rw" \
		"2 1 0:2 / $fixture/.git/objects/nested\040mount rw - tmpfs tmpfs rw" \
		>"$fixture/mountinfo"
	LD_PRELOAD="$fixture/mount-table.so" VFSI_TEST_MOUNTINFO="$fixture/mountinfo" \
		VFSI_TEST_OPEN_FAIL=1 VFSI_TRACE=1 VFSI_IMPL=nfs VFSI_LIBRARY="$fixture/proxy.so" \
		VFSI_HOST=127.0.0.1 VFSI_EXPORT=/ VFSI_MOUNT="$fixture" \
		"$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
	diff -u "$fixture/expected" "$fixture/actual"
	if grep -Eq 'vfsi: (open failed|listdirs completed)' "$fixture/trace"; then exit 1; fi
fi
# Metadata acceleration remains useful without a byte-limited payload reader.
${CC:-cc} -shared -fPIC -DVFSI_TEST_NO_PREFETCH -I"$include" \
    "$repo/contrib/vfsi/test-proxy.c" -ldl -o "$fixture/metadata-only.so"
${CC:-cc} -shared -fPIC -DVFSI_TEST_LEGACY_PREFETCH -I"$include" \
    "$repo/contrib/vfsi/test-proxy.c" -ldl -o "$fixture/legacy-prefetch.so"
VFSI_IMPL=off "$binary" -C "$fixture" fsck --full --no-dangling >"$fixture/native-fsck"
for adapter in metadata-only.so legacy-prefetch.so; do
    VFSI_TEST_CALL_LOG="$fixture/legacy-prefetch-calls" VFSI_TRACE=1 VFSI_IMPL=dummy \
	VFSI_LIBRARY="$fixture/$adapter" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	VFSI_PREFETCH_OBJECTS=1 VFSI_PREFETCH_BYTES=128 \
	"$binary" -C "$fixture" fsck --full --no-dangling >"$fixture/actual" 2>"$fixture/trace"
    diff -u "$fixture/native-fsck" "$fixture/actual"
    grep -q 'vfsi: listdirs completed' "$fixture/trace"
    test ! -e "$fixture/legacy-prefetch-calls"
done
# Invalid cached timestamps must choose ordinary reads with initialized stat data.
${CC:-cc} -shared -fPIC -DVFSI_TEST_NO_IDENTITY -I"$include" "$repo/contrib/vfsi/test-proxy.c" -ldl -o "$fixture/no-identity.so"
VFSI_TRACE=1 VFSI_PREFETCH_OBJECTS=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/no-identity.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
diff -u "$fixture/expected" "$fixture/actual"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
VFSI_TEST_BAD_ID=1 VFSI_TRACE=1 VFSI_PREFETCH_OBJECTS=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/proxy.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" fsck --full 2>"$fixture/trace"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
VFSI_TEST_BAD_TIME=1 VFSI_TRACE=1 VFSI_PREFETCH_OBJECTS=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/proxy.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" fsck --full 2>"$fixture/trace"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
VFSI_IMPL=off "$binary" -C "$fixture" cat-file --batch-all-objects --batch-check >"$fixture/native-objects"
VFSI_TEST_BAD_TIME=1 VFSI_TRACE=1 VFSI_PREFETCH_OBJECTS=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/proxy.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" cat-file --batch-all-objects --batch-check >"$fixture/vector-objects" 2>"$fixture/trace"
diff -u "$fixture/native-objects" "$fixture/vector-objects"
# Freshen an unreachable object after READDIR but before prune's age check.
oid=$(printf 'unreachable regression object\n' | "$binary" -C "$fixture" hash-object -w --stdin)
object="$fixture/.git/objects/$(printf '%s' "$oid" | cut -c1-2)/$(printf '%s' "$oid" | cut -c3-)"
touch -d '2000-01-01 UTC' "$object"
VFSI_TEST_FRESHEN="$object" VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/proxy.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" prune --expire=1.hour.ago 2>"$fixture/trace"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
test -f "$object"
"$binary" -C "$fixture" cat-file -e "$oid"
printf '%s\n' 'Loader, partial-listing fallback, and concurrent freshening regressions passed.'

# Special objects must retain native classification rather than becoming files.
mkdir -p "$fixture/.git/objects/ab"
fifo="$fixture/.git/objects/ab/00000000000000000000000000000000000000"
mkfifo "$fifo"
VFSI_IMPL=off "$binary" -C "$fixture" count-objects -v >"$fixture/expected" 2>"$fixture/reference-errors"
VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
diff -u "$fixture/expected" "$fixture/actual"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
rm "$fifo"
printf '%s\n' 'Special-file classification regression passed.'

# Enumerate and read alternate objects and release each alternate's session
# before advancing to the next directory.
for alternate in alternate-a alternate-b; do
    "$binary" -C "$fixture" init -q "$alternate"
    printf 'object contents in %s\n' "$alternate" |
	"$binary" -C "$fixture/$alternate" hash-object -w --stdin >/dev/null
done
mkdir -p "$fixture/.git/objects/info"
printf '%s\n' "$fixture/alternate-a/.git/objects" "$fixture/alternate-b/.git/objects" \
    >"$fixture/.git/objects/info/alternates"
VFSI_IMPL=off "$binary" -C "$fixture" cat-file --batch-all-objects --batch >"$fixture/native-alternates"
: >"$fixture/sessions"
for adapter in "$library" "$fixture/proxy.so"; do
    VFSI_TEST_SESSION_LOG="$fixture/sessions" VFSI_TRACE=1 VFSI_PREFETCH_OBJECTS=1 \
	VFSI_IMPL=dummy VFSI_LIBRARY="$adapter" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	"$binary" -C "$fixture" cat-file --batch-all-objects --batch >"$fixture/vector-alternates" 2>"$fixture/trace"
    diff -u "$fixture/native-alternates" "$fixture/vector-alternates"
    test "$(grep -c 'vfsi: listdirs completed' "$fixture/trace")" -ge 3
done
awk '
$1 == "open" {
	if (++opened == 1) { primary = $2; next }
	if (alternate || $2 == primary) exit 1
	alternate = $2
}
$1 == "free" {
	if ($2 == primary) {
		if (released != 2 || primary_released) exit 1
		primary_released = 1
	} else {
		if ($2 != alternate) exit 1
		alternate = ""
		released++
	}
}
END { if (opened != 3 || released != 2 || alternate) exit 1 }
' "$fixture/sessions"
printf '%s\n' 'Alternate object enumeration, reads, and session cleanup passed.'

# Cursor-specific fault proxies must never export an operation absent from the
# real library. Keep legacy parity above active for old headers and libraries.
cat >"$fixture/cursor-api.c" <<'C'
#include <vfsi.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
#ifdef VFSI_DIRECTORY_CURSOR_IDS_VERSION
    void *handle = argc == 2 ? dlopen(argv[1], RTLD_NOW | RTLD_LOCAL) : 0;
    int available = handle && dlsym(handle, "vfsi_listdirs_from_cursors_with_ids") &&
	dlsym(handle, "vfsi_dir_cursor_free");
    if (handle) dlclose(handle);
    return !available;
#else
    (void)argc; (void)argv;
    return 1;
#endif
}
C
${CC:-cc} -Wall -Wextra -Werror -I"$include" "$fixture/cursor-api.c" -ldl -o "$fixture/cursor-api"
if "$fixture/cursor-api" "$library"; then
rm "$fixture/.git/objects/info/alternates"
# More than two fanout groups expose prefix replay, per-group budget resets,
# and old-library/partial-symbol fallback. Empty fanouts still count as groups.
for n in $(seq 0 255); do
    fanout=$(printf '%02x' "$n")
    mkdir -p "$fixture/.git/objects/$fanout"
done
for n in $(seq 1 96); do
    printf 'bounded fanout object %s\n' "$n" | "$binary" -C "$fixture" hash-object -w --stdin >/dev/null
done
# The normal implementation visits lowercase fanouts only.
mkdir -p "$fixture/.git/objects/AB"
printf 'not an object\n' > "$fixture/.git/objects/AB/garbage"
VFSI_IMPL=off "$binary" -C "$fixture" count-objects -v >"$fixture/expected"
${CC:-cc} -shared -fPIC -DVFSI_TEST_CURSOR_API -I"$include" \
    "$repo/contrib/vfsi/test-proxy.c" -ldl -o "$fixture/cursors.so"
for failure in 0 1 2 11; do
    : >"$fixture/calls"
    VFSI_TEST_GROUP_FAIL=$failure VFSI_TEST_CALL_LOG="$fixture/calls" \
	VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/cursors.so" \
	VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" VFSI_PREFETCH_OBJECTS=1 VFSI_PREFETCH_BYTES=512 \
	"$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
    diff -u "$fixture/expected" "$fixture/actual"
    grep -q 'vfsi: fanout cursors=retained' "$fixture/trace"
    test "$(grep -c '^group ' "$fixture/calls")" -ge 3
    awk '$1=="group" && $3>24 {exit 1}
	 $1=="read-bytes" {exit 1}' "$fixture/calls"
    if test "$failure" -ne 0; then
	grep -q 'vfsi: fanout group fallback' "$fixture/trace"
    fi
done
# A connection loss keeps the completed prefix and uses POSIX for all remaining fanouts.
for fault in VFSI_TEST_GROUP_TIMEOUT VFSI_TEST_GROUP_IO; do
    : >"$fixture/calls"
    env "$fault=2" VFSI_TEST_CALL_LOG="$fixture/calls" \
	VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/cursors.so" \
	VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	"$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
    diff -u "$fixture/expected" "$fixture/actual"
    test "$(grep -c '^group ' "$fixture/calls")" -eq 2
    test "$(grep -c 'vfsi: fanout cursors=retained' "$fixture/trace")" -eq 1
    grep -q 'vfsi: fanout group fallback .*replay=unreplayed-suffix' "$fixture/trace"
done
# An SDK success that leaves an incomplete group is a local validation failure.
: >"$fixture/calls"
VFSI_TEST_GROUP_INCOMPLETE=2 VFSI_TEST_CALL_LOG="$fixture/calls" \
    VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/cursors.so" \
    VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
diff -u "$fixture/expected" "$fixture/actual"
test "$(grep -c '^group ' "$fixture/calls")" -ge 3
grep -q 'vfsi: fanout group fallback .*replay=unreplayed-group' "$fixture/trace"
# Content consumers still share one prefetch budget across fanout groups.
: >"$fixture/calls"
VFSI_TEST_CALL_LOG="$fixture/calls" VFSI_TRACE=1 VFSI_IMPL=dummy \
    VFSI_LIBRARY="$fixture/cursors.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    VFSI_PREFETCH_OBJECTS=1 VFSI_PREFETCH_BYTES=512 \
    "$binary" -C "$fixture" fsck --full --no-dangling >"$fixture/actual" 2>"$fixture/trace"
grep -q 'vfsi: fanout cursors=retained' "$fixture/trace"
awk '$1=="read-bytes" {bytes+=$2} END {if(bytes==0 || bytes>512) exit 1}' "$fixture/calls"
# A cursor-listing export without its ownership/free companion must use paths.
${CC:-cc} -shared -fPIC -DVFSI_TEST_CURSOR_API -DVFSI_TEST_NO_CURSOR_FREE -I"$include" \
    "$repo/contrib/vfsi/test-proxy.c" -ldl -o "$fixture/partial-cursors.so"
for adapter in proxy.so partial-cursors.so; do
    VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/$adapter" \
	VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	"$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
    diff -u "$fixture/expected" "$fixture/actual"
    grep -q 'vfsi: listdirs completed' "$fixture/trace"
    if grep -q 'vfsi: fanout cursors=retained' "$fixture/trace"; then exit 1; fi
done
printf '%s\n' 'Retained fanout, bounded groups, late fallback and shared prefetch budget passed.'
elif test "${VFSI_REQUIRE_CURSOR_API:-0}" = 1; then
    printf '%s\n' 'The published VFSI SDK must expose the retained cursor listing and free functions.' >&2
    exit 1
else
    printf '%s\n' 'Cursor-specific cases unavailable; legacy library parity and fallback passed.'
fi

# Prune-packed and the maintenance scans only enumerate names and object IDs.
# Keep a reachable duplicate loose so the dry run actually traverses it.
"$binary" -C "$fixture" pack-objects --all "$fixture/.git/objects/pack/pack" </dev/null >/dev/null
VFSI_IMPL=off "$binary" -C "$fixture" prune-packed -n >"$fixture/native-prune"
grep -q '^rm -f ' "$fixture/native-prune"
VFSI_TEST_CALL_LOG="$fixture/metadata-reads" VFSI_TRACE=1 VFSI_IMPL=dummy \
    VFSI_LIBRARY="$fixture/proxy.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    VFSI_PREFETCH_OBJECTS=1 \
    "$binary" -C "$fixture" prune-packed -n >"$fixture/vector-prune" 2>"$fixture/trace"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
sort "$fixture/native-prune" >"$fixture/native-prune-sorted"
sort "$fixture/vector-prune" >"$fixture/vector-prune-sorted"
diff -u "$fixture/native-prune-sorted" "$fixture/vector-prune-sorted"
test ! -e "$fixture/metadata-reads"

VFSI_TEST_CALL_LOG="$fixture/metadata-reads" VFSI_TRACE=1 VFSI_IMPL=dummy \
    VFSI_LIBRARY="$fixture/proxy.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    VFSI_PREFETCH_OBJECTS=1 \
    "$binary" -C "$fixture" -c maintenance.loose-objects.auto=1 \
    maintenance run --auto --task=loose-objects 2>"$fixture/trace"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
test ! -e "$fixture/metadata-reads"
