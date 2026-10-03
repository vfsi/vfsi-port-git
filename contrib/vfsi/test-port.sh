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
    VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
        VFSI_PREFETCH_OBJECTS=1 VFSI_PREFETCH_BYTES=$budget \
        "$binary" -C "$fixture" count-objects -v > "$fixture/actual"
    diff -u "$fixture/expected" "$fixture/actual"
    VFSI_IMPL=dummy VFSI_LIBRARY="$library" VFSI_ROOT="$fixture" VFSI_MOUNT="$fixture" \
        VFSI_PREFETCH_OBJECTS=1 VFSI_PREFETCH_BYTES=$budget \
        "$binary" -C "$fixture" fsck --full
done
printf '%s\n' 'Git port parity passed (zero, tiny and normal prefetch budgets).'
