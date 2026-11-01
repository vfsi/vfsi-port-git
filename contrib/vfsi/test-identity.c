#include "object-identity.h"
#include <assert.h>

int main(void)
{
	struct vfsi_identity_map map = { 0 };
	struct vfsi_object_id a = {
		.struct_size = sizeof(a), .version = VFSI_OBJECT_ID_VERSION,
		.namespace_id = 1, .fs_id = { UINT64_MAX, 9 }, .fileid = 42
	};
	struct stat first = { 0 }, alias = { 0 }, other = { 0 };
	assert(!vfsi_identity_stat(&map, &a, &first));
	assert(first.st_dev && first.st_ino == 42);
	/* Same full object through hard-link/mount aliases and later snapshots. */
	assert(!vfsi_identity_stat(&map, &a, &alias));
	assert(first.st_dev == alias.st_dev && first.st_ino == alias.st_ino);
	a.fs_id[0]--;
	assert(!vfsi_identity_stat(&map, &a, &other));
	assert(first.st_dev != other.st_dev && first.st_ino == other.st_ino);
	a.fs_id[0]++;
	a.namespace_id++;
	assert(!vfsi_identity_stat(&map, &a, &other));
	assert(first.st_dev != other.st_dev && first.st_ino == other.st_ino);
	a.namespace_id = 1;
	assert(!vfsi_identity_stat(&map, &a, &alias));
	assert(first.st_dev == alias.st_dev && map.nr == 3);
	a.version++;
	assert(vfsi_identity_stat(&map, &a, &other) == EPROTO);
	a.version--;
	a.namespace_id = 0;
	assert(vfsi_identity_stat(&map, &a, &other) == EPROTO);
	assert(map.nr == 3);
	free(map.devices);
	return 0;
}
