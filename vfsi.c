#include "git-compat-util.h"
#include "abspath.h"
#include "gettext.h"
#include "hex.h"
#include "object-file.h"
#include "strbuf.h"
#include "strvec.h"
#include "vfsi.h"

#include <dlfcn.h>

/* Minimal ABI mirrors of vfsi-c/include/vfsi.h. */
struct vfsi_fs;

struct vfsi_attrs {
	uint32_t ftype;
	uint32_t mode;
	uint64_t size;
	uint32_t nlink;
	uint64_t fileid;
	uint32_t uid;
	uint32_t gid;
	uint64_t blocks;
	int64_t atime_sec;
	uint32_t atime_nsec;
	int64_t mtime_sec;
	uint32_t mtime_nsec;
	int64_t ctime_sec;
	uint32_t ctime_nsec;
};

typedef int (*vfsi_listdir_cb)(const char *name,
			       const struct vfsi_attrs *attrs,
			       void *userdata);
typedef int (*vfsi_listdirv_cb)(const char *dir,
				const char *name,
				const struct vfsi_attrs *attrs,
				void *userdata);
typedef int (*vfsi_read_paths_cb)(const char *path,
				  const unsigned char *data,
				  size_t len,
				  void *userdata);

struct vfsi_bindings {
	void *handle;
	int (*dummy_open_mount)(const char *, const char *, struct vfsi_fs **);
	int (*nfs_open_mount)(const char *, const char *, struct vfsi_fs **);
	int (*listdir)(struct vfsi_fs *, const char *, vfsi_listdir_cb, void *);
	int (*listdirv)(struct vfsi_fs *, const char *const *, size_t,
			size_t, int, vfsi_listdirv_cb, void *);
	int (*read_paths)(struct vfsi_fs *, const char *const *, size_t,
			  vfsi_read_paths_cb, void *);
	void (*free)(struct vfsi_fs *);
};

static struct vfsi_context {
	struct vfsi_bindings bindings;
	struct vfsi_fs *fs;
	char *mountpoint;
	int cleanup_registered;
} vfsi_ctx;

enum {
	VFSI_NF4DIR = 2,
	VFSI_NF4REG = 1,
	VFSI_NF4LNK = 5
};

struct vfsi_object {
	char *display_dir;
	char *display_path;
	char *name;
	char *real_path;
	struct vfsi_attrs attrs;
	unsigned char *data;
	size_t data_len;
};

static struct vfsi_object *vfsi_objects;
static size_t vfsi_objects_nr;
static size_t vfsi_objects_cap;

static int vfsi_prefetch_enabled(void)
{
	const char *value = getenv("VFSI_PREFETCH_OBJECTS");

	return value && *value && strcmp(value, "0");
}

static void vfsi_clear_objects(void)
{
	size_t i;

	for (i = 0; i < vfsi_objects_nr; i++) {
		free(vfsi_objects[i].display_dir);
		free(vfsi_objects[i].display_path);
		free(vfsi_objects[i].name);
		free(vfsi_objects[i].real_path);
		free(vfsi_objects[i].data);
	}
	vfsi_objects_nr = 0;
}

static void vfsi_cleanup(void)
{
	vfsi_clear_objects();
	free(vfsi_objects);
	vfsi_objects = NULL;
	vfsi_objects_cap = 0;
	if (vfsi_ctx.fs && vfsi_ctx.bindings.free)
		vfsi_ctx.bindings.free(vfsi_ctx.fs);
	vfsi_ctx.fs = NULL;
	/*
	 * Leave libvfsi_c (and its libntirpc worker pool) mapped until the
	 * process exits. dlclose() here unloads the NFS runtime while its
	 * background threads are still tearing down, which can crash inside
	 * tirpc_free() during exit.
	 */
	free(vfsi_ctx.mountpoint);
	vfsi_ctx.mountpoint = NULL;
}

static int load_symbol(void *handle, const char *name, void **out)
{
	void *sym = dlsym(handle, name);

	if (!sym) {
		warning(_("vfsi: cannot load %s: %s"), name, dlerror());
		return -1;
	}
	*out = sym;
	return 0;
}

static int vfsi_mountpoint_for(const char *path, char **mountpoint_out)
{
	FILE *fp;
	char line[4096];
	char *best = NULL;
	size_t best_len = 0;

	fp = fopen("/proc/self/mounts", "r");
	if (!fp)
		return 0;
	while (fgets(line, sizeof(line), fp)) {
		char *p = line;
		char *mount = NULL;
		char *fstype = NULL;
		size_t len;

		while (*p == ' ' || *p == '\t')
			p++;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		while (*p == ' ' || *p == '\t')
			p++;
		mount = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (*p) {
			*p++ = '\0';
			while (*p == ' ' || *p == '\t')
				p++;
			fstype = p;
			while (*p && *p != ' ' && *p != '\t')
				p++;
			if (*p)
				*p = '\0';
		}
		if (!mount || !fstype)
			continue;
		if (strcmp(fstype, "nfs") && strcmp(fstype, "nfs4"))
			continue;
		len = strlen(mount);
		if (len <= best_len || !starts_with(path, mount) ||
		    (path[len] != '/' && path[len] != '\0'))
			continue;
		free(best);
		best = xstrdup(mount);
		best_len = len;
	}
	fclose(fp);
	*mountpoint_out = best;
	return best != NULL;
}

static int open_vfsi(const char *objects_path)
{
	struct vfsi_bindings *b = &vfsi_ctx.bindings;
	const char *impl = getenv("VFSI_IMPL");
	const char *library = getenv("VFSI_LIBRARY");
	char *mountpoint = NULL;
	const char *host;
	const char *dummy_root;
	const char *dummy_mount;
	int rc;

	if (!impl || !strcmp(impl, "off") || !library)
		return 0;
	if (strcmp(impl, "nfs") && strcmp(impl, "dummy"))
		return 0;

	if (!b->handle) {
		b->handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
		if (!b->handle) {
			warning(_("vfsi: cannot load %s: %s"), library, dlerror());
			return 0;
		}
#define LOAD_VFSI(name, field) do { \
	void *sym; \
	if (load_symbol(b->handle, name, &sym) < 0) { \
		vfsi_cleanup(); \
		return 0; \
	} \
	memcpy(&(field), &sym, sizeof(sym)); \
} while (0)
		LOAD_VFSI("vfsi_dummy_open_mount", b->dummy_open_mount);
		LOAD_VFSI("vfsi_nfs_open_mount", b->nfs_open_mount);
	LOAD_VFSI("vfsi_listdir", b->listdir);
	LOAD_VFSI("vfsi_listdirv", b->listdirv);
	LOAD_VFSI("vfsi_read_paths", b->read_paths);
	LOAD_VFSI("vfsi_free", b->free);
#undef LOAD_VFSI
		if (!vfsi_ctx.cleanup_registered) {
			atexit(vfsi_cleanup);
			vfsi_ctx.cleanup_registered = 1;
		}
	}

	if (!strcmp(impl, "dummy")) {
		dummy_root = getenv("VFSI_ROOT");
		dummy_mount = getenv("VFSI_MOUNT");
		if (!dummy_root || !dummy_mount) {
			vfsi_cleanup();
			return 0;
		}
		if (vfsi_ctx.fs) {
			b->free(vfsi_ctx.fs);
			vfsi_ctx.fs = NULL;
		}
		free(vfsi_ctx.mountpoint);
		vfsi_ctx.mountpoint = NULL;
		vfsi_ctx.mountpoint = xstrdup(dummy_mount);
		rc = b->dummy_open_mount(dummy_root, dummy_mount, &vfsi_ctx.fs);
	} else {
		if (!vfsi_mountpoint_for(objects_path, &mountpoint)) {
			vfsi_cleanup();
			return 0;
		}
		if (vfsi_ctx.fs) {
			b->free(vfsi_ctx.fs);
			vfsi_ctx.fs = NULL;
		}
		free(vfsi_ctx.mountpoint);
		vfsi_ctx.mountpoint = mountpoint;
		host = getenv("VFSI_HOST");
		rc = b->nfs_open_mount(host ? host : "127.0.0.1",
				       vfsi_ctx.mountpoint, &vfsi_ctx.fs);
	}
	if (rc) {
		warning(_("vfsi: open failed: %d"), rc);
		return -1;
	}
	return 1;
}

struct vfsi_walk {
	const struct git_hash_algo *algop;
	each_loose_object_fn *obj_cb;
	each_loose_cruft_fn *cruft_cb;
	each_loose_subdir_fn *subdir_cb;
	void *data;
	struct strvec subdirs;
	struct strvec display_subdirs;
	const char *display_root;
	const char *real_root;
	int error;
	int stop;
};

static int is_hexpair(const char *s)
{
	return isxdigit((unsigned char)s[0]) &&
	       isxdigit((unsigned char)s[1]) && s[2] == '\0';
}

static int cmp_subdir_ptr(const void *a, const void *b)
{
	const char *const *pa = a;
	const char *const *pb = b;

	return strcmp(*pa, *pb);
}

static int collect_subdir_cb(const char *name, const struct vfsi_attrs *attrs,
			     void *userdata)
{
	struct vfsi_walk *walk = userdata;

	if (attrs->ftype != VFSI_NF4DIR || !is_hexpair(name))
		return 1;
	strvec_pushf(&walk->subdirs, "%s/%s", walk->real_root, name);
	strvec_pushf(&walk->display_subdirs, "%s/%s",
		     walk->display_root, name);
	return 1;
}

static int process_loose_entry(struct vfsi_walk *walk,
			       const char *dir, const char *name)
{
	struct object_id oid;
	const struct git_hash_algo *algop = walk->algop;
	size_t namelen = strlen(name);
	char *path;
	int rc = 0;

	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return 0;
	path = xstrfmt("%s/%s", dir, name);
	if (namelen == algop->hexsz - 2) {
		unsigned subdir_nr = (unsigned)hexval_table[(unsigned char)dir[strlen(dir) - 2]];

		subdir_nr = (subdir_nr << 4) |
			    hexval_table[(unsigned char)dir[strlen(dir) - 1]];
		oid.hash[0] = subdir_nr;
		if (!hex_to_bytes(oid.hash + 1, name, algop->rawsz - 1)) {
			oid_set_algo(&oid, algop);
			memset(oid.hash + algop->rawsz, 0,
			       GIT_MAX_RAWSZ - algop->rawsz);
			if (walk->obj_cb)
				rc = walk->obj_cb(&oid, path, walk->data);
		} else if (walk->cruft_cb) {
			rc = walk->cruft_cb(name, path, walk->data);
		}
	} else if (walk->cruft_cb) {
		rc = walk->cruft_cb(name, path, walk->data);
	}
	free(path);
	if (rc) {
		walk->error = rc;
		walk->stop = 1;
	}
	return rc;
}

static const char *display_dir_for_real(struct vfsi_walk *walk,
					const char *dir)
{
	size_t i;

	for (i = 0; i < walk->subdirs.nr; i++)
		if (!strcmp(walk->subdirs.v[i], dir))
			return walk->display_subdirs.v[i];
	return NULL;
}

static struct vfsi_object *vfsi_object_add(const char *display_dir,
					   const char *display_path,
					   const char *real_dir,
					   const char *name,
					   const struct vfsi_attrs *attrs)
{
	struct vfsi_object *obj;

	ALLOC_GROW(vfsi_objects, vfsi_objects_nr + 1, vfsi_objects_cap);
	obj = &vfsi_objects[vfsi_objects_nr++];
	memset(obj, 0, sizeof(*obj));
	obj->display_dir = xstrdup(display_dir);
	obj->display_path = xstrdup(display_path);
	obj->name = xstrdup(name);
	obj->real_path = xstrfmt("%s/%s", real_dir, name);
	obj->attrs = *attrs;
	return obj;
}

static int loose_entry_cb(const char *dir, const char *name,
			  const struct vfsi_attrs *attrs,
			  void *userdata)
{
	struct vfsi_walk *walk = userdata;
	const char *display_dir = display_dir_for_real(walk, dir);
	char *display_path;

	if (!display_dir)
		return 1;
	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return 1;
	display_path = xstrfmt("%s/%s", display_dir, name);
	vfsi_object_add(display_dir, display_path, dir, name, attrs);
	free(display_path);
	return 1;
}

static struct vfsi_object *vfsi_find_object(const char *path)
{
	size_t i;

	for (i = 0; i < vfsi_objects_nr; i++) {
		if (!strcmp(vfsi_objects[i].display_path, path) ||
		    !strcmp(vfsi_objects[i].real_path, path))
			return &vfsi_objects[i];
	}
	return NULL;
}

static int vfsi_store_read_paths_cb(const char *path, const unsigned char *data,
				    size_t len, void *userdata)
{
	struct vfsi_object *obj = vfsi_find_object(path);

	(void)userdata;
	if (!obj)
		return 1;
	free(obj->data);
	obj->data = xmalloc(len ? len : 1);
	memcpy(obj->data, data, len);
	obj->data_len = len;
	return 1;
}

static int vfsi_prefetch_object_data(void)
{
	const char **paths;
	size_t i;
	size_t next = 0;
	size_t chunk;
	int rc = 0;

	if (!vfsi_objects_nr)
		return 0;
	paths = xcalloc(vfsi_objects_nr, sizeof(*paths));
	for (i = 0; i < vfsi_objects_nr; i++)
		paths[next++] = vfsi_objects[i].real_path;
	for (chunk = 0; chunk < next; chunk += 4096) {
		size_t count = next - chunk < 4096 ? next - chunk : 4096;

		rc = vfsi_ctx.bindings.read_paths(vfsi_ctx.fs,
						  (const char *const *)&paths[chunk],
						  count, vfsi_store_read_paths_cb,
						  NULL);
		if (rc)
			break;
	}
	free(paths);
	return rc;
}

static int object_cmp(const void *a, const void *b)
{
	const struct vfsi_object *oa = a;
	const struct vfsi_object *ob = b;
	int cmp;

	cmp = strcmp(oa->display_dir, ob->display_dir);
	if (cmp)
		return cmp;
	return strcmp(oa->name, ob->name);
}

static void vfsi_sort_objects(void)
{
	qsort(vfsi_objects, vfsi_objects_nr, sizeof(*vfsi_objects),
	      object_cmp);
}

static void vfsi_attrs_to_stat(const struct vfsi_attrs *attrs, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_ino = attrs->fileid;
	st->st_mode = attrs->mode & 07777;
	if (attrs->ftype == VFSI_NF4DIR)
		st->st_mode |= S_IFDIR;
	else if (attrs->ftype == VFSI_NF4LNK)
		st->st_mode |= S_IFLNK;
	else
		st->st_mode |= S_IFREG;
	st->st_nlink = attrs->nlink;
	st->st_uid = attrs->uid;
	st->st_gid = attrs->gid;
	st->st_size = attrs->size;
	st->st_blocks = attrs->blocks;
	st->st_atim.tv_sec = attrs->atime_sec;
	st->st_atim.tv_nsec = attrs->atime_nsec;
	st->st_mtim.tv_sec = attrs->mtime_sec;
	st->st_mtim.tv_nsec = attrs->mtime_nsec;
	st->st_ctim.tv_sec = attrs->ctime_sec;
	st->st_ctim.tv_nsec = attrs->ctime_nsec;
}

int vfsi_fill_stat(const char *path, struct stat *st)
{
	struct vfsi_object *obj = vfsi_find_object(path);

	if (!obj)
		return 0;
	vfsi_attrs_to_stat(&obj->attrs, st);
	return 1;
}

int vfsi_read_loose_object(const char *path, void **buf, unsigned long *size)
{
	struct vfsi_object *obj = vfsi_find_object(path);

	if (!obj || !obj->data)
		return 0;
	*buf = xmalloc(obj->data_len ? obj->data_len : 1);
	memcpy(*buf, obj->data, obj->data_len);
	*size = obj->data_len;
	return 1;
}

static int vfsi_for_each_loose_file_locked(const char *objects_dir,
					   const struct git_hash_algo *algop,
					   each_loose_object_fn obj_cb,
					   each_loose_cruft_fn cruft_cb,
					   each_loose_subdir_fn subdir_cb,
					   void *data)
{
	struct vfsi_walk walk = {
		.algop = algop,
		.obj_cb = obj_cb,
		.cruft_cb = cruft_cb,
		.subdir_cb = subdir_cb,
		.data = data,
		.display_root = objects_dir,
	};
	char *real_objects;
	size_t i;
	const char **dirs = NULL;
	int rc;

	real_objects = real_pathdup(objects_dir, 0);
	if (!real_objects)
		return 0;
	vfsi_clear_objects();
	walk.real_root = real_objects;
	strvec_init(&walk.subdirs);
	strvec_init(&walk.display_subdirs);
	rc = open_vfsi(real_objects);
	if (rc <= 0) {
		strvec_clear(&walk.subdirs);
		strvec_clear(&walk.display_subdirs);
		free(real_objects);
		return rc;
	}

	rc = vfsi_ctx.bindings.listdir(vfsi_ctx.fs, real_objects,
				       collect_subdir_cb, &walk);
	if (rc) {
		error(_("vfsi: unable to list %s"), real_objects);
		goto fail;
	}
	if (!walk.subdirs.nr)
		goto done;

	dirs = xcalloc(walk.subdirs.nr, sizeof(*dirs));
	for (i = 0; i < walk.subdirs.nr; i++)
		dirs[i] = walk.subdirs.v[i];
	/* Match the normal implementation's numeric subdirectory order
	 * (objects/00 … objects/ff), which fsck relies on when it learns
	 * object types while scanning. */
	qsort(dirs, walk.subdirs.nr, sizeof(*dirs), cmp_subdir_ptr);
	rc = vfsi_ctx.bindings.listdirv(vfsi_ctx.fs, dirs, walk.subdirs.nr,
					0, 0, loose_entry_cb, &walk);
	free(dirs);
	dirs = NULL;
	if (rc) {
		error(_("vfsi: listdirv failed"));
		goto fail;
	}
	if (vfsi_prefetch_enabled() && vfsi_prefetch_object_data() < 0) {
		error(_("vfsi: unable to prefetch loose objects"));
		goto fail;
	}
	vfsi_sort_objects();
	for (i = 0; i < vfsi_objects_nr; i++) {
		struct vfsi_object *obj = &vfsi_objects[i];
		int obj_rc;

		if (walk.stop)
			break;
		obj_rc = process_loose_entry(&walk, obj->display_dir, obj->name);
		if (obj_rc && !walk.stop)
			break;
	}
	if (walk.stop)
		goto fail;

	for (i = 0; i < walk.subdirs.nr; i++) {
		const char *dir = walk.display_subdirs.v[i];
		const char *slash = strrchr(dir, '/');
		unsigned nr;

		if (!slash || !subdir_cb)
			continue;
		nr = (unsigned)hexval_table[(unsigned char)slash[1]];
		nr = (nr << 4) | hexval_table[(unsigned char)slash[2]];
		if (subdir_cb(nr, dir, data))
			break;
	}

done:
	rc = 1;
	goto out;
fail:
	rc = -1;
out:
	strvec_clear(&walk.subdirs);
	strvec_clear(&walk.display_subdirs);
	free(real_objects);
	free(dirs);
	return rc;
}

int vfsi_for_each_loose_file(const char *objects_dir,
			     const struct git_hash_algo *algop,
			     each_loose_object_fn obj_cb,
			     each_loose_cruft_fn cruft_cb,
			     each_loose_subdir_fn subdir_cb,
			     void *data)
{
	return vfsi_for_each_loose_file_locked(objects_dir, algop,
					       obj_cb, cruft_cb,
					       subdir_cb, data);
}
