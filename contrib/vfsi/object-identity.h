/* Private Git cache projection of the full, scoped VFSI identity.
 * Device values are collision-free within this process's VFSI cache, not
 * persistent kernel device numbers. Never hash or truncate the 128-bit FSID.
 * The caller serializes map access; the active SDK stays loaded for its life.
 */
#ifndef GIT_VFSI_IDENTITY_H
#define GIT_VFSI_IDENTITY_H
#include <vfsi.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

struct vfsi_identity_device {
	uint64_t namespace_id;
	uint64_t fs_id[2];
};
struct vfsi_identity_map {
	struct vfsi_identity_device *devices;
	size_t nr, alloc;
};

static inline int vfsi_identity_stat(struct vfsi_identity_map *map,
		const struct vfsi_object_id *id, struct stat *st)
{
	size_t i;
	dev_t device;
	ino_t inode;
	if (id->version != VFSI_OBJECT_ID_VERSION || id->struct_size < sizeof(*id) || !id->namespace_id)
		return EPROTO;
	inode = (ino_t)id->fileid;
	if ((uintmax_t)inode != id->fileid)
		return EOVERFLOW;
	for (i = 0; i < map->nr; i++)
		if (map->devices[i].namespace_id == id->namespace_id &&
		    map->devices[i].fs_id[0] == id->fs_id[0] &&
		    map->devices[i].fs_id[1] == id->fs_id[1])
			break;
	if (i == SIZE_MAX || !(device = (dev_t)(i + 1)) || (uintmax_t)device != i + 1)
		return EOVERFLOW;
	if (i == map->nr) {
		if (map->nr == map->alloc) {
			size_t alloc = map->alloc ? map->alloc * 2 : 4;
			void *devices;
			if (alloc < map->alloc || alloc > SIZE_MAX / sizeof(*map->devices))
				return EOVERFLOW;
			devices = realloc(map->devices, alloc * sizeof(*map->devices));
			if (!devices)
				return ENOMEM;
			map->devices = devices;
			map->alloc = alloc;
		}
		map->devices[i].namespace_id = id->namespace_id;
		map->devices[i].fs_id[0] = id->fs_id[0];
		map->devices[i].fs_id[1] = id->fs_id[1];
		map->nr++;
	}
	st->st_dev = device;
	st->st_ino = inode;
	return 0;
}
#endif
