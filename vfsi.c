#include "git-compat-util.h"
#include "abspath.h"
#include "gettext.h"
#include "hex.h"
#include "object-file.h"
#include "strbuf.h"
#include "strvec.h"
#include "vfsi.h"

#include <dlfcn.h>
#include <pthread.h>

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

struct vfsi_bindings {
	void *handle;
	int (*dummy_open_mount)(const char *, const char *, struct vfsi_fs **);
	int (*nfs_open_mount)(const char *, const char *, struct vfsi_fs **);
	int (*listdir)(struct vfsi_fs *, const char *, vfsi_listdir_cb, void *);
	int (*listdirv)(struct vfsi_fs *, const char *const *, size_t,
			size_t, int, vfsi_listdirv_cb, void *);
	void (*free)(struct vfsi_fs *);
};

static struct vfsi_context {
	struct vfsi_bindings bindings;
	struct vfsi_fs *fs;
	char *mountpoint;
	int cleanup_registered;
} vfsi_ctx;

static pthread_mutex_t vfsi_lock = PTHREAD_MUTEX_INITIALIZER;

enum {
	VFSI_NF4DIR = 2
};

static void vfsi_cleanup(void)
{
	if (vfsi_ctx.fs && vfsi_ctx.bindings.free)
		vfsi_ctx.bindings.free(vfsi_ctx.fs);
	vfsi_ctx.fs = NULL;
	if (vfsi_ctx.bindings.handle)
		dlclose(vfsi_ctx.bindings.handle);
	vfsi_ctx.bindings.handle = NULL;
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
		if (vfsi_ctx.fs && vfsi_ctx.mountpoint &&
		    !strcmp(vfsi_ctx.mountpoint, dummy_mount))
			return 1;
		if (vfsi_ctx.fs) {
			b->free(vfsi_ctx.fs);
			vfsi_ctx.fs = NULL;
		}
		free(vfsi_ctx.mountpoint);
		vfsi_ctx.mountpoint = xstrdup(dummy_mount);
		rc = b->dummy_open_mount(dummy_root, dummy_mount, &vfsi_ctx.fs);
	} else {
		if (!vfsi_mountpoint_for(objects_path, &mountpoint)) {
			vfsi_cleanup();
			return 0;
		}
		if (vfsi_ctx.fs && vfsi_ctx.mountpoint &&
		    !strcmp(vfsi_ctx.mountpoint, mountpoint)) {
			free(mountpoint);
			return 1;
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

static int loose_entry_cb(const char *dir, const char *name,
			  const struct vfsi_attrs *attrs UNUSED,
			  void *userdata)
{
	struct vfsi_walk *walk = userdata;
	const char *display_dir = display_dir_for_real(walk, dir);
	int rc;

	if (!display_dir)
		return 1;
	rc = process_loose_entry(walk, display_dir, name);
	return rc == 0;
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
	const char **dirs;
	int rc;

	real_objects = real_pathdup(objects_dir, 0);
	if (!real_objects)
		return 0;
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

	dirs = walk.subdirs.v;
	rc = vfsi_ctx.bindings.listdirv(vfsi_ctx.fs, dirs, walk.subdirs.nr,
					0, 0, loose_entry_cb, &walk);
	if (rc) {
		error(_("vfsi: listdirv failed"));
		goto fail;
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
	return rc;
}

int vfsi_for_each_loose_file(const char *objects_dir,
			     const struct git_hash_algo *algop,
			     each_loose_object_fn obj_cb,
			     each_loose_cruft_fn cruft_cb,
			     each_loose_subdir_fn subdir_cb,
			     void *data)
{
	int rc;

	pthread_mutex_lock(&vfsi_lock);
	rc = vfsi_for_each_loose_file_locked(objects_dir, algop,
					     obj_cb, cruft_cb,
					     subdir_cb, data);
	pthread_mutex_unlock(&vfsi_lock);
	return rc;
}
