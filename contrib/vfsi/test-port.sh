#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
binary="$repo/git"
library=${VFSI_LIBRARY:?set VFSI_LIBRARY to the development adapter}
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
    diff -u "$fixture/expected" "$fixture/actual"
    VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	VFSI_PREFETCH_OBJECTS=1 VFSI_PREFETCH_BYTES=$budget \
	"$binary" -C "$fixture" fsck --full
done
printf '%s\n' 'Git port parity passed (zero, tiny and normal prefetch budgets).'

# A loaded-but-incompatible adapter must not silently masquerade as acceleration.
include=${VFSI_INCLUDE:-"$repo/build-vfsi-c/vfsi-c-0.3.9-vnfs-0.0.26/source/include"}
${CC:-cc} -Wall -Wextra -Werror -I"$include" -I"$repo/contrib/vfsi" \
    "$repo/contrib/vfsi/test-identity.c" -o "$fixture/test-identity"
"$fixture/test-identity"
${CC:-cc} -shared -fPIC -I"$include" "$repo/contrib/vfsi/test-proxy.c" -ldl -o "$fixture/proxy.so"
export VFSI_TEST_REAL_LIBRARY="$library"
for fault in VFSI_TEST_BAD_ABI VFSI_TEST_PARTIAL; do
    env "$fault=1" VFSI_TRACE=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/proxy.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
	"$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
    diff -u "$fixture/expected" "$fixture/actual"
    if grep -q 'vfsi: listdirs completed' "$fixture/trace"; then exit 1; fi
done
# Metadata acceleration remains useful when an adapter lacks prefetch exports.
${CC:-cc} -shared -fPIC -DVFSI_TEST_NO_PREFETCH -I"$include" "$repo/contrib/vfsi/test-proxy.c" -ldl -o "$fixture/metadata-only.so"
VFSI_TRACE=1 VFSI_PREFETCH_OBJECTS=1 VFSI_IMPL=dummy VFSI_LIBRARY="$fixture/metadata-only.so" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" count-objects -v >"$fixture/actual" 2>"$fixture/trace"
diff -u "$fixture/expected" "$fixture/actual"
grep -q 'vfsi: listdirs completed' "$fixture/trace"
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

# The files backend now owns a directory list rather than a source chain.
# Enumerating and reading alternate objects must still reach each loose source.
"$binary" -C "$fixture" init -q alternate
printf 'alternate object contents\n' | "$binary" -C "$fixture/alternate" hash-object -w --stdin >/dev/null
mkdir -p "$fixture/.git/objects/info"
printf '%s\n' "$fixture/alternate/.git/objects" >"$fixture/.git/objects/info/alternates"
VFSI_IMPL=off "$binary" -C "$fixture" cat-file --batch-all-objects --batch >"$fixture/native-alternates"
VFSI_TRACE=1 VFSI_PREFETCH_OBJECTS=1 VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
    "$binary" -C "$fixture" cat-file --batch-all-objects --batch >"$fixture/vector-alternates" 2>"$fixture/trace"
diff -u "$fixture/native-alternates" "$fixture/vector-alternates"
test "$(grep -c 'vfsi: listdirs completed' "$fixture/trace")" -ge 2
printf '%s\n' 'Alternate object-directory enumeration and reads passed.'
