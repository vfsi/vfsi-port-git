#include "git-compat-util.h"
#include "abspath.h"
#include "gettext.h"
#include "hex.h"
#include "object-file.h"
#include "odb/source-files.h"
#include "odb/source-loose.h"
#include "strbuf.h"
#include "strmap.h"
#include "strvec.h"
#include "thread-utils.h"
#include "git-vfsi.h"
#include <vfsi.h>
#include <vfsi_dynamic.h>
#include <vfsi_posix.h>
#include "contrib/vfsi/object-identity.h"

#if VFSI_ABI_VERSION != 3
#error "Git VFSI support requires VFSI C ABI v3"
#endif

#include <dlfcn.h>

enum vfsi_loader_state {
	VFSI_LOADER_UNINITIALIZED,
	VFSI_LOADER_AVAILABLE,
	VFSI_LOADER_UNAVAILABLE,
};

static struct {
	vfsi_dynamic_api bindings;
	enum vfsi_loader_state loader_state;
	pthread_mutex_t mutex;
	struct vfsi_identity_map identity_map;
} vfsi_runtime = { .mutex = PTHREAD_MUTEX_INITIALIZER };

enum {
	VFSI_NF4DIR = 2,
	VFSI_NF4REG = 1,
	VFSI_NF4LNK = 5
};

struct vfsi_object {
	const char *display_dir;
	char *display_path;
	const char *name;
	char *real_path;
	struct vfsi_attrs attrs;
	struct vfsi_object_id identity;
	unsigned char *data;
	size_t data_len;
};

struct vfsi_source_context {
	struct vfsi_fs *fs;
	char *mountpoint;
	char *connection_key;
	struct vfsi_object *objects;
	size_t objects_nr;
	size_t objects_cap;
	struct strmap objects_by_path;
	struct strmap objects_by_dir;
	size_t data_bytes;
	size_t data_limit;
	pthread_mutex_t mutex;
	int traversal_active;
};

struct vfsi_object_bucket {
	size_t *indices;
	size_t nr, alloc;
};

static int vfsi_prefetch_enabled(void)
{
	const char *value = getenv("VFSI_PREFETCH_OBJECTS");

	return value && *value && strcmp(value, "0");
}

static void vfsi_clear_objects(struct vfsi_source_context *ctx)
{
	size_t i;
	struct hashmap_iter iter;
	struct strmap_entry *entry;
	strmap_for_each_entry(&ctx->objects_by_dir, &iter, entry) {
		struct vfsi_object_bucket *bucket = entry->value;
		free(bucket->indices);
		free(bucket);
	}
	strmap_clear(&ctx->objects_by_dir, 0);

	strmap_clear(&ctx->objects_by_path, 0);
	for (i = 0; i < ctx->objects_nr; i++) {
		free(ctx->objects[i].display_path);
		free(ctx->objects[i].real_path);
		free(ctx->objects[i].data);
	}
	ctx->objects_nr = 0;
	ctx->data_bytes = 0;
}

static struct odb_source_loose *vfsi_loose_source(struct odb_source *source)
{
	if (!source)
		return NULL;
	if (source->type == ODB_SOURCE_FILES) {
		struct odb_files_dir *dir = odb_source_files_downcast(source)->dirs;
		return dir ? dir->loose : NULL;
	}
	if (source->type == ODB_SOURCE_LOOSE)
		return odb_source_loose_downcast(source);
	return NULL;
}

static struct vfsi_source_context *vfsi_source_context(struct odb_source *source,
							int create)
{
	struct odb_source_loose *loose = vfsi_loose_source(source);
	struct vfsi_source_context *ctx;

	if (!loose)
		return NULL;
	pthread_mutex_lock(&vfsi_runtime.mutex);
	ctx = loose->vfsi;
	if (!ctx && create) {
		CALLOC_ARRAY(ctx, 1);
		/* Object paths outlive the index; clear it before freeing objects. */
		strmap_init_with_options(&ctx->objects_by_path, NULL, 0);
		strmap_init(&ctx->objects_by_dir);
		if (init_recursive_mutex(&ctx->mutex))
			die_errno(_("vfsi: cannot initialize source mutex"));
		loose->vfsi = ctx;
	}
	pthread_mutex_unlock(&vfsi_runtime.mutex);
	return ctx;
}

void vfsi_source_invalidate(struct odb_source *source)
{
	struct vfsi_source_context *ctx = vfsi_source_context(source, 0);

	if (!ctx)
		return;
	pthread_mutex_lock(&ctx->mutex);
	/* Callback replay owns a stable snapshot and clears it before returning. */
	if (!ctx->traversal_active)
		vfsi_clear_objects(ctx);
	pthread_mutex_unlock(&ctx->mutex);
}

void vfsi_source_close(struct odb_source *source)
{
	struct vfsi_source_context *ctx = vfsi_source_context(source, 0);

	if (!ctx)
		return;
	pthread_mutex_lock(&ctx->mutex);
	vfsi_clear_objects(ctx);
	if (ctx->fs && vfsi_runtime.bindings.free)
		vfsi_runtime.bindings.free(ctx->fs);
	ctx->fs = NULL;
	FREE_AND_NULL(ctx->mountpoint);
	FREE_AND_NULL(ctx->connection_key);
	pthread_mutex_unlock(&ctx->mutex);
}

void vfsi_source_release(struct odb_source *source)
{
	struct odb_source_loose *loose = vfsi_loose_source(source);
	struct vfsi_source_context *ctx;

	if (!loose || !loose->vfsi)
		return;
	ctx = loose->vfsi;
	vfsi_source_close(source);
	free(ctx->objects);
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
	loose->vfsi = NULL;
}

static int load_vfsi_bindings(const char *library)
{
	vfsi_dynamic_api candidate = { 0 };
	int result;

	pthread_mutex_lock(&vfsi_runtime.mutex);
	if (vfsi_runtime.loader_state == VFSI_LOADER_AVAILABLE) {
		result = 1;
		goto out;
	}
	if (vfsi_runtime.loader_state == VFSI_LOADER_UNAVAILABLE) {
		result = 0;
		goto out;
	}

	candidate.handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
	if (!candidate.handle) {
		warning(_("vfsi: cannot load %s: %s"), library, dlerror());
		goto unavailable;
	}
	if (vfsi_dynamic_load(&candidate, candidate.handle) ||
	    !candidate.listdir || !candidate.listdirs)
		goto unavailable;

	vfsi_runtime.bindings = candidate;
	vfsi_runtime.loader_state = VFSI_LOADER_AVAILABLE;
	result = 1;
	goto out;

unavailable:
	if (candidate.handle)
		dlclose(candidate.handle);
	vfsi_runtime.loader_state = VFSI_LOADER_UNAVAILABLE;
	result = 0;
out:
	pthread_mutex_unlock(&vfsi_runtime.mutex);
	return result;
}

struct vfsi_mount {
	char *mountpoint;
	char *host;
	char *export_root;
};

static void vfsi_mount_clear(struct vfsi_mount *mount)
{
	free(mount->mountpoint);
	free(mount->host);
	free(mount->export_root);
	memset(mount, 0, sizeof(*mount));
}

static void collect_mount(const char *host, const char *export_root, const char *point, void *data)
{
	struct vfsi_mount *mount = data;
	vfsi_mount_clear(mount);
	mount->host = xstrdup(host);
	mount->export_root = xstrdup(export_root);
	mount->mountpoint = xstrdup(point);
}

static int vfsi_mount_for(const char *path, struct vfsi_mount *result)
{
	if (!vfsi_runtime.bindings.nfs_mount_info ||
		vfsi_runtime.bindings.nfs_mount_info(path, collect_mount, result))
		return 0;
	if (!result->host || !result->export_root || !result->mountpoint) {
		vfsi_mount_clear(result);
		return 0;
	}
	return 1;
}

/* Reuse only the same source namespace, credentials, and explicit settings.
 * Object snapshots are still cleared independently at each traversal boundary. */
static char *vfsi_connection_key(const char *path)
{
    struct strbuf key = STRBUF_INIT;
    struct stat st;
    gid_t *groups;
    int count, i;
    const char *settings[] = { "VFSI_IMPL", "VFSI_HOST", "VFSI_EXPORT", "VFSI_MOUNT", "VFSI_ROOT" };
    if (stat(path, &st) || (count = getgroups(0, NULL)) < 0)
	return NULL;
    CALLOC_ARRAY(groups, count ? count : 1);
    if (getgroups(count, groups) != count) { free(groups); return NULL; }
    strbuf_addf(&key, "%s:%"PRIuMAX":%"PRIuMAX":%"PRIuMAX":%"PRIuMAX, path,
	(uintmax_t)st.st_dev, (uintmax_t)st.st_ino, (uintmax_t)geteuid(), (uintmax_t)getegid());
    for (i = 0; i < count; i++) strbuf_addf(&key, ":%"PRIuMAX, (uintmax_t)groups[i]);
    free(groups);
    for (i = 0; i < ARRAY_SIZE(settings); i++) {
	const char *value = getenv(settings[i]);
	strbuf_addf(&key, ":%zu:", value ? strlen(value) : 0);
	if (value) strbuf_addstr(&key, value);
    }
    return strbuf_detach(&key, NULL);
}

static int open_vfsi(struct vfsi_source_context *ctx, const char *objects_path)
{
	vfsi_dynamic_api *b = &vfsi_runtime.bindings;
	const char *impl = getenv("VFSI_IMPL");
	const char *library = getenv("VFSI_LIBRARY");
	struct vfsi_mount mount = { 0 };
	const char *host_override;
	const char *export_override;
	const char *mount_override;
	const char *dummy_root;
	const char *dummy_mount;
	int rc;
	char *key;

	if (!impl || !strcmp(impl, "off") || !library)
		return 0;
	if (strcmp(impl, "nfs") && strcmp(impl, "dummy"))
		return 0;

	if (!load_vfsi_bindings(library))
		return 0;

	key = vfsi_connection_key(objects_path);
	if (!key) return 0;
	if (ctx->fs && ctx->connection_key && !strcmp(key, ctx->connection_key)) {
		free(key);
		return 1;
	}
	if (ctx->fs) vfsi_runtime.bindings.free(ctx->fs);
	ctx->fs = NULL;
	FREE_AND_NULL(ctx->connection_key);
	ctx->connection_key = key;
	if (!strcmp(impl, "dummy")) {
		dummy_root = getenv("VFSI_ROOT");
		dummy_mount = getenv("VFSI_MOUNT");
		if (!b->dummy_open_mount || !dummy_root || !dummy_mount)
			return 0;
		if (ctx->fs) {
			b->free(ctx->fs);
			ctx->fs = NULL;
		}
		free(ctx->mountpoint);
		ctx->mountpoint = xstrdup(dummy_mount);
		rc = b->dummy_open_mount(dummy_root, dummy_mount, &ctx->fs);
	} else {
		host_override = getenv("VFSI_HOST");
		export_override = getenv("VFSI_EXPORT");
		mount_override = getenv("VFSI_MOUNT");
		if (!host_override && !export_override && !mount_override) {
			/* Default discovery inherits security and rejects covering mounts.
			 * An old adapter cannot do that safely: keep Git's POSIX path. */
			if (!b->nfs_from_mount)
				return 0;
			if (ctx->fs) b->free(ctx->fs);
			ctx->fs = NULL;
			rc = b->nfs_from_mount(objects_path, &ctx->fs);
			if (!rc && getenv("VFSI_TRACE")) fprintf(stderr, "vfsi: connected\n");
			return rc ? 0 : 1;
		}
		if (!b->nfs_open_mount_export) return 0;
		if (!vfsi_mount_for(objects_path, &mount)) {
			if (!mount_override || !export_override)
				return 0;
			mount.mountpoint = xstrdup(mount_override);
			mount.export_root = xstrdup(export_override);
			mount.host = xstrdup(host_override ? host_override : "127.0.0.1");
		}
		if (host_override) {
			free(mount.host);
			mount.host = xstrdup(host_override);
		}
		if (export_override) {
			free(mount.export_root);
			mount.export_root = xstrdup(export_override);
		}
		if (mount_override) {
			free(mount.mountpoint);
			mount.mountpoint = xstrdup(mount_override);
		}
		if (ctx->fs) {
			b->free(ctx->fs);
			ctx->fs = NULL;
		}
		free(ctx->mountpoint);
		ctx->mountpoint = xstrdup(mount.mountpoint);
		rc = b->nfs_open_mount_export(mount.host, mount.export_root,
					      mount.mountpoint, &ctx->fs);
		vfsi_mount_clear(&mount);
	}
	if (rc) {
		warning(_("vfsi: open failed: %d"), rc);
		return -1;
	}
	if (getenv("VFSI_TRACE")) fprintf(stderr, "vfsi: connected\n");
	return 1;
}

struct vfsi_walk {
	struct vfsi_source_context *ctx;
	const struct git_hash_algo *algop;
	each_loose_object_fn *obj_cb;
	each_loose_cruft_fn *cruft_cb;
	each_loose_subdir_fn *subdir_cb;
	void *data;
	struct strvec subdirs;
	struct strvec display_subdirs;
	const char *display_root;
	const char *real_root;
	const char **ordered_display_dirs;
	size_t batch_count;
#ifdef VFSI_DIRECTORY_CURSOR_IDS_VERSION
	struct vfsi_dir_cursor *seeds[256];
#endif
};

static int vfsi_attrs_valid(const struct vfsi_attrs *attrs)
{
	return attrs && attrs->abi_version == VFSI_ABI_VERSION &&
		attrs->struct_size >= sizeof(*attrs);
}

static int is_hexpair(const char *s)
{
	return s[0] && s[1] && s[2] == '\0' &&
	       strchr("0123456789abcdef", s[0]) &&
	       strchr("0123456789abcdef", s[1]);
}

static int cmp_subdir_ptr(const void *a, const void *b)
{
	const char *const *pa = a;
	const char *const *pb = b;

	return strcmp(*pa, *pb);
}

static bool collect_subdir_cb(const char *name, const struct vfsi_attrs *attrs,
			      void *userdata)
{
	struct vfsi_walk *walk = userdata;

	if (!name || !vfsi_attrs_valid(attrs))
		return false;
	if (attrs->ftype != VFSI_NF4DIR || !is_hexpair(name))
		return true;
	strvec_pushf(&walk->subdirs, "%s/%s", walk->real_root, name);
	strvec_pushf(&walk->display_subdirs, "%s/%s",
		     walk->display_root, name);
	return true;
}

#ifdef VFSI_DIRECTORY_CURSOR_IDS_VERSION
static bool collect_subdir_cursor_cb(size_t index, const char *dir, const char *name,
				     const struct vfsi_attrs *attrs,
				     const struct vfsi_object_id *id,
				     struct vfsi_dir_cursor *child, void *userdata)
{
	struct vfsi_walk *walk = userdata;
	size_t before = walk->subdirs.nr;
	int ok = index == 0 && dir && id && collect_subdir_cb(name, attrs, userdata);
	if (ok && walk->subdirs.nr > before) {
		unsigned nr = (hexval_table[(unsigned char)name[0]] << 4) |
			hexval_table[(unsigned char)name[1]];
		if (walk->seeds[nr]) {
			vfsi_runtime.bindings.dir_cursor_free(child);
			return false;
		}
		walk->seeds[nr] = child;
	} else {
		vfsi_runtime.bindings.dir_cursor_free(child);
	}
	return ok;
}

static bool loose_entry_cursor_ids_cb(size_t index, const char *dir, const char *name,
				     const struct vfsi_attrs *attrs,
				     const struct vfsi_object_id *id,
				     struct vfsi_dir_cursor *child, void *userdata);
#endif

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

static struct vfsi_object *vfsi_object_add(struct vfsi_source_context *ctx,
					   const char *display_dir,
					   char *display_path,
					   const char *real_dir,
					   const char *name,
					   const struct vfsi_attrs *attrs)
{
	struct vfsi_object *obj;
	size_t index = ctx->objects_nr;
	void *index_value;

	ALLOC_GROW(ctx->objects, ctx->objects_nr + 1, ctx->objects_cap);
	obj = &ctx->objects[ctx->objects_nr++];
	memset(obj, 0, sizeof(*obj));
	obj->display_path = display_path;
	obj->name = strrchr(obj->display_path, '/') + 1;
	obj->real_path = xstrfmt("%s/%s", real_dir, name);
	obj->attrs = *attrs;
	index_value = (void *)(uintptr_t)(index + 1);
	strmap_put(&ctx->objects_by_path, obj->display_path, index_value);
	strmap_put(&ctx->objects_by_path, obj->real_path, index_value);
	{
		struct vfsi_object_bucket *bucket = strmap_get(&ctx->objects_by_dir, display_dir);
		if (!bucket) {
			CALLOC_ARRAY(bucket, 1);
			strmap_put(&ctx->objects_by_dir, display_dir, bucket);
		}
		obj->display_dir = strmap_get_entry(&ctx->objects_by_dir, display_dir)->key;
		ALLOC_GROW(bucket->indices, bucket->nr + 1, bucket->alloc);
		bucket->indices[bucket->nr++] = index;
	}
	return obj;
}

static bool loose_entry_cb(size_t index, const char *dir, const char *name,
			   const struct vfsi_attrs *attrs,
			   void *userdata)
{
	struct vfsi_walk *walk = userdata;
	const char *display_dir = index < walk->batch_count ? walk->ordered_display_dirs[index] : NULL;
	char *display_path;
	if (!dir || !name || !vfsi_attrs_valid(attrs))
		return false;
	if (!display_dir)
		return false;
	if (!strcmp(name, ".") || !strcmp(name, ".."))
		return true;
	display_path = xstrfmt("%s/%s", display_dir, name);
	vfsi_object_add(walk->ctx, display_dir, display_path, dir, name, attrs);
	return true;
}

#ifdef VFSI_LISTDIR_IDS_VERSION
static bool loose_entry_ids_cb(size_t index, const char *dir, const char *name,
			      const struct vfsi_attrs *attrs,
			      const struct vfsi_object_id *id, void *userdata)
{
	struct vfsi_walk *walk = userdata;
	size_t before = walk->ctx->objects_nr;
	if (!attrs || !vfsi_attrs_valid(attrs) || !id || id->version != VFSI_OBJECT_ID_VERSION ||
		id->struct_size < sizeof(*id) || !id->namespace_id ||
		id->fileid != attrs->fileid)
		return false;
	if (!loose_entry_cb(index, dir, name, attrs, userdata))
		return false;
	if (walk->ctx->objects_nr > before)
		walk->ctx->objects[before].identity = *id;
	return true;
}
#endif

#ifdef VFSI_DIRECTORY_CURSOR_IDS_VERSION
static bool loose_entry_cursor_ids_cb(size_t index, const char *dir, const char *name,
				     const struct vfsi_attrs *attrs,
				     const struct vfsi_object_id *id,
				     struct vfsi_dir_cursor *child, void *userdata)
{
	/* Loose-object scans never descend into object entries. */
	vfsi_runtime.bindings.dir_cursor_free(child);
	return loose_entry_ids_cb(index, dir, name, attrs, id, userdata);
}
#endif

static struct vfsi_object *vfsi_find_object(struct vfsi_source_context *ctx,
					    const char *path)
{
	void *index_value = strmap_get(&ctx->objects_by_path, path);
	size_t index;

	if (!index_value)
		return NULL;
	index = (uintptr_t)index_value - 1;
	if (index >= ctx->objects_nr)
		BUG("vfsi path index is out of bounds");
	return &ctx->objects[index];
}

static bool vfsi_store_read_paths_cb(const char *path,
				     const unsigned char *data,
				     size_t len, void *userdata)
{
	struct vfsi_source_context *ctx = userdata;
	struct vfsi_object *obj = vfsi_find_object(ctx, path);
	if (!obj)
		return true;
	if (len > ctx->data_limit - ctx->data_bytes)
		return false;
	free(obj->data);
	obj->data = xmalloc(len ? len : 1);
	memcpy(obj->data, data, len);
	obj->data_len = len;
	ctx->data_bytes += len;
	return true;
}

static int vfsi_prefetch_budget(size_t *limit)
{
	const char *setting = getenv("VFSI_PREFETCH_BYTES");
	*limit = 64 * 1024 * 1024;
	if (setting) {
		char *end;
		unsigned long long value;
		errno = 0;
		value = strtoull(setting, &end, 10);
		if (errno || *end || !*setting || *setting == '-' || value > SIZE_MAX)
			return -1;
		*limit = (size_t)value;
	}

	return 0;
}

static int vfsi_prefetch_object_data(struct vfsi_source_context *ctx, size_t *remaining)
{
	const char **paths;
	size_t i, next = 0, chunk, planned = 0;
	int rc = 0;
	ctx->data_limit = *remaining;

	if (!ctx->objects_nr)
		return 0;
	CALLOC_ARRAY(paths, ctx->objects_nr);
	for (i = 0; i < ctx->objects_nr; i++) {
		struct vfsi_object *obj = &ctx->objects[i];
		const char *name = obj->name;
		size_t j, len = strlen(name);
		if (obj->attrs.ftype != VFSI_NF4REG || (len != 38 && len != 62) ||
		    obj->attrs.size > ctx->data_limit - planned)
			continue;
		for (j = 0; j < len && hexval_table[(unsigned char)name[j]] >= 0; j++) {}
		if (j != len)
			continue;
		planned += obj->attrs.size;
		paths[next++] = obj->real_path;
	}
	for (chunk = 0; chunk < next; chunk += 4096) {
		size_t count = next - chunk < 4096 ? next - chunk : 4096;

		if (vfsi_runtime.bindings.read_paths_with_limit)
			rc = vfsi_runtime.bindings.read_paths_with_limit(ctx->fs,
				(const char *const *)&paths[chunk], count,
				ctx->data_limit - ctx->data_bytes, vfsi_store_read_paths_cb, ctx);
		else
			rc = vfsi_runtime.bindings.read_paths(ctx->fs,
						  (const char *const *)&paths[chunk],
						  count, vfsi_store_read_paths_cb,
						  ctx);
		if (rc)
			break;
	}
	free(paths);
	*remaining -= ctx->data_bytes;
	return rc;
}

static void vfsi_refresh_object_ids(struct vfsi_source_context *ctx)
{
	size_t start;
	if (!vfsi_runtime.bindings.get_object_idv)
		return;
	for (start = 0; start < ctx->objects_nr; start += 4096) {
		size_t i, count = ctx->objects_nr - start;
		struct vfsi_object_id_op *ops;
		struct vfsi_result *results;
		struct vfsi_result batch;
		if (count > 4096)
			count = 4096;
		CALLOC_ARRAY(ops, count);
		CALLOC_ARRAY(results, count);
		for (i = 0; i < count; i++) {
			struct vfsi_object *obj = &ctx->objects[start + i];
			obj->identity.struct_size = sizeof(obj->identity);
			obj->identity.version = VFSI_OBJECT_ID_VERSION;
			ops[i].path = obj->real_path;
			ops[i].fd = -1;
			ops[i].flags = VFSI_OBJECT_ID_NOFOLLOW;
			ops[i].out = &obj->identity;
			ops[i].attrs = &obj->attrs;
		}
		batch = vfsi_runtime.bindings.get_object_idv(ctx->fs, ops, count, results);
		for (i = 0; i < count; i++)
			if (batch.category != VFSI_ERROR_NONE ||
			    results[i].category != VFSI_ERROR_NONE)
				memset(&ctx->objects[start + i].identity, 0, sizeof(ctx->objects[start + i].identity));
		free(results);
		free(ops);
	}
}

static int vfsi_attrs_to_stat(const struct vfsi_attrs *attrs,
			    const struct vfsi_object_id *id, struct stat *st)
{
	int error;
	if (!attrs || !id || attrs->fileid != id->fileid ||
		vfsi_attrs_stat_fields(attrs, st))
		return 0;
	pthread_mutex_lock(&vfsi_runtime.mutex);
	error = vfsi_identity_stat(&vfsi_runtime.identity_map, id, st);
	pthread_mutex_unlock(&vfsi_runtime.mutex);
	return !error;
}

int vfsi_fill_stat(struct odb_source *source, const char *path, struct stat *st)
{
	struct vfsi_source_context *ctx = vfsi_source_context(source, 0);
	struct vfsi_object *obj;
	int valid;

	if (!ctx)
		return 0;
	pthread_mutex_lock(&ctx->mutex);
	obj = vfsi_find_object(ctx, path);
	if (!obj) {
		pthread_mutex_unlock(&ctx->mutex);
		return 0;
	}
	valid = vfsi_attrs_to_stat(&obj->attrs, &obj->identity, st);
	pthread_mutex_unlock(&ctx->mutex);
	return valid;
}

int vfsi_read_loose_object(struct odb_source *source, const char *path,
			   void **buf, unsigned long *size, struct stat *st)
{
	struct vfsi_source_context *ctx;
	struct vfsi_object *obj;

	if (source && source->type == ODB_SOURCE_FILES) {
		struct odb_files_dir *dir;

		for (dir = odb_source_files_downcast(source)->dirs; dir; dir = dir->next)
			if (vfsi_read_loose_object(&dir->loose->base, path, buf, size, st))
				return 1;
		return 0;
	}
	ctx = vfsi_source_context(source, 0);
	if (!ctx)
		return 0;
	pthread_mutex_lock(&ctx->mutex);
	obj = vfsi_find_object(ctx, path);
	if (!obj || !obj->data) {
		pthread_mutex_unlock(&ctx->mutex);
		return 0;
	}
	if (st && !vfsi_attrs_to_stat(&obj->attrs, &obj->identity, st)) {
		if (getenv("VFSI_TRACE")) fprintf(stderr, "vfsi: cached read metadata unavailable\n");
		pthread_mutex_unlock(&ctx->mutex);
		return 0;
	}
	*buf = obj->data;
	*size = obj->data_len;
	ctx->data_bytes -= obj->data_len;
	obj->data = NULL;
	obj->data_len = 0;
	pthread_mutex_unlock(&ctx->mutex);
	return 1;
}

static int vfsi_for_each_loose_file_locked(struct vfsi_source_context *ctx,
					   const char *objects_dir,
					   const struct git_hash_algo *algop,
					   each_loose_object_fn obj_cb,
					   each_loose_cruft_fn cruft_cb,
					   each_loose_subdir_fn subdir_cb,
					   void *data, int *result)
{
	struct vfsi_walk walk = {
		.ctx = ctx,
		.algop = algop,
		.obj_cb = obj_cb,
		.cruft_cb = cruft_cb,
		.subdir_cb = subdir_cb,
		.data = data,
		.display_root = objects_dir,
	};
	char *real_objects;
	size_t i, start, prefetch_remaining = 0;
	const char **dirs = NULL, **display_dirs = NULL;
	int rc, use_cursors = 0;
	int prefetch;
	struct vfsi_listing_options options = {
		.max_entries = 200000,
		.max_path_bytes = 64 * 1024 * 1024,
		.attributes = VFSI_ATTR_MODE | VFSI_ATTR_SIZE | VFSI_ATTR_NLINK |
			VFSI_ATTR_FILEID | VFSI_ATTR_BLOCKS | VFSI_ATTR_UID |
			VFSI_ATTR_GID | VFSI_ATTR_ATIME | VFSI_ATTR_MTIME | VFSI_ATTR_CTIME
	};

	*result = 0;
	real_objects = real_pathdup(objects_dir, 0);
	if (!real_objects)
		return 0;
	vfsi_clear_objects(ctx);
	walk.real_root = real_objects;
	strvec_init(&walk.subdirs);
	strvec_init(&walk.display_subdirs);
	rc = open_vfsi(ctx, real_objects);
	if (rc <= 0)
		goto out;
	prefetch = vfsi_prefetch_enabled() &&
		(vfsi_runtime.bindings.read_paths || vfsi_runtime.bindings.read_paths_with_limit);
	if (prefetch && vfsi_prefetch_budget(&prefetch_remaining)) {
		warning(_("vfsi: invalid prefetch budget; using ordinary reads"));
		prefetch = 0;
	}
#ifdef VFSI_DIRECTORY_CURSOR_IDS_VERSION
	use_cursors = vfsi_runtime.bindings.listdirs_from_cursors_with_ids &&
		vfsi_runtime.bindings.dir_cursor_free;
	if (use_cursors) {
		struct vfsi_dir_cursor *cursor = NULL;
		struct vfsi_listing_result listing = { 0 };
		const char *root = real_objects;
		rc = vfsi_runtime.bindings.listdirs_from_cursors_with_ids(ctx->fs,
			&root, &cursor, 1, &options, collect_subdir_cursor_cb, &walk, &listing);
		vfsi_runtime.bindings.dir_cursor_free(cursor);
		if (!rc && listing.completion != 1)
			rc = EIO;
	} else
#endif
		rc = vfsi_runtime.bindings.listdir(ctx->fs, real_objects, collect_subdir_cb, &walk);
	if (rc) {
		/* Read-only staging has not invoked any Git callbacks. */
		rc = 0;
		goto out;
	}
	if (!walk.subdirs.nr) {
		rc = 1;
		goto out;
	}

	CALLOC_ARRAY(dirs, walk.subdirs.nr);
	CALLOC_ARRAY(display_dirs, walk.subdirs.nr);
	for (i = 0; i < walk.subdirs.nr; i++)
		dirs[i] = walk.subdirs.v[i];
	/* Match the normal implementation's numeric fanout order. */
	QSORT(dirs, walk.subdirs.nr, cmp_subdir_ptr);
	for (i = 0; i < walk.subdirs.nr; i++)
		display_dirs[i] = display_dir_for_real(&walk, dirs[i]);

	/* Stage, validate, replay, and release one bounded directory group.
	 * Never replay a completed prefix if a later group's staging fails. */
	for (start = 0; start < walk.subdirs.nr; start += 24) {
		struct vfsi_listing_result results[24] = { { 0 } };
		size_t count = walk.subdirs.nr - start;
		int listed_ids = 0;
		if (count > ARRAY_SIZE(results))
			count = ARRAY_SIZE(results);
		walk.batch_count = count;
		walk.ordered_display_dirs = display_dirs + start;
		vfsi_clear_objects(ctx);
#ifdef VFSI_DIRECTORY_CURSOR_IDS_VERSION
		if (use_cursors) {
			struct vfsi_dir_cursor *cursors[24] = { NULL };
			for (i = 0; i < count; i++) {
				const char *name = strrchr(dirs[start + i], '/') + 1;
				unsigned nr = (hexval_table[(unsigned char)name[0]] << 4) |
					hexval_table[(unsigned char)name[1]];
				cursors[i] = walk.seeds[nr];
				walk.seeds[nr] = NULL;
			}
			listed_ids = 1;
			rc = vfsi_runtime.bindings.listdirs_from_cursors_with_ids(ctx->fs,
				dirs + start, cursors, count, &options,
				loose_entry_cursor_ids_cb, &walk, results);
			for (i = 0; i < count; i++)
				vfsi_runtime.bindings.dir_cursor_free(cursors[i]);
		} else
#endif
#ifdef VFSI_LISTDIR_IDS_VERSION
		if (vfsi_runtime.bindings.listdirs_with_ids) {
			listed_ids = 1;
			rc = vfsi_runtime.bindings.listdirs_with_ids(ctx->fs, dirs + start, count,
				&options, loose_entry_ids_cb, &walk, results);
		} else
#endif
			rc = vfsi_runtime.bindings.listdirs(ctx->fs, dirs + start, count,
				&options, loose_entry_cb, &walk, results);
		for (i = 0; !rc && i < count; i++)
			if (results[i].completion != 1)
				rc = EIO;
		if (rc) {
			struct strbuf path = STRBUF_INIT;
			vfsi_clear_objects(ctx);
			strbuf_addstr(&path, objects_dir);
			if (getenv("VFSI_TRACE"))
				fprintf(stderr, "vfsi: fanout group fallback start=%zu count=%zu\n", start, count);
			for (i = 0; i < count; i++) {
				const char *name = strrchr(dirs[start + i], '/') + 1;
				unsigned nr = (hexval_table[(unsigned char)name[0]] << 4) |
					hexval_table[(unsigned char)name[1]];
				*result = for_each_file_in_obj_subdir(nr, &path, algop,
					obj_cb, cruft_cb, subdir_cb, data);
				if (*result)
					break;
			}
			strbuf_release(&path);
			if (*result)
				goto handled;
			continue;
		}
		if (getenv("VFSI_TRACE")) {
			fprintf(stderr, "vfsi: listdirs completed entries=%zu\n", ctx->objects_nr);
			if (listed_ids)
				fprintf(stderr, "vfsi: listing identities=inline\n");
			if (use_cursors)
				fprintf(stderr, "vfsi: fanout cursors=retained count=%zu\n", count);
		}
		if (!listed_ids)
			vfsi_refresh_object_ids(ctx);
		if (prefetch && prefetch_remaining && vfsi_prefetch_object_data(ctx, &prefetch_remaining))
			warning(_("vfsi: object prefetch unavailable; using ordinary reads"));
		for (i = 0; i < count; i++) {
			const char *dir = display_dirs[start + i];
			const char *name = strrchr(dir, '/') + 1;
			size_t j;
			unsigned nr;
			struct vfsi_object_bucket *bucket = strmap_get(&ctx->objects_by_dir, dir);
			for (j = 0; bucket && j < bucket->nr; j++) {
				struct vfsi_object *obj = &ctx->objects[bucket->indices[j]];
				*result = process_loose_entry(&walk, obj->display_dir, obj->name);
				if (*result)
					goto handled;
			}
			if (!subdir_cb)
				continue;
			nr = (hexval_table[(unsigned char)name[0]] << 4) |
				hexval_table[(unsigned char)name[1]];
			*result = subdir_cb(nr, dir, data);
			if (*result)
				goto handled;
		}
	}
handled:
	rc = 1;
out:
#ifdef VFSI_DIRECTORY_CURSOR_IDS_VERSION
	if (vfsi_runtime.bindings.dir_cursor_free)
		for (i = 0; i < ARRAY_SIZE(walk.seeds); i++)
			vfsi_runtime.bindings.dir_cursor_free(walk.seeds[i]);
#endif
	strvec_clear(&walk.subdirs);
	strvec_clear(&walk.display_subdirs);
	free(real_objects);
	free(dirs);
	free(display_dirs);
	return rc;
}

int vfsi_for_each_loose_file(struct odb_source *source,
			     const char *objects_dir,
			     const struct git_hash_algo *algop,
			     each_loose_object_fn obj_cb,
			     each_loose_cruft_fn cruft_cb,
			     each_loose_subdir_fn subdir_cb,
			     void *data, int *result)
{
	struct vfsi_source_context *ctx;
	int rc;

	if (!result)
		BUG("vfsi_for_each_loose_file requires a result pointer");
	ctx = vfsi_source_context(source, 1);
	if (!ctx)
		return 0;
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->traversal_active) {
		pthread_mutex_unlock(&ctx->mutex);
		return 0;
	}
	ctx->traversal_active = 1;
	rc = vfsi_for_each_loose_file_locked(ctx, objects_dir, algop,
					    obj_cb, cruft_cb,
					    subdir_cb, data, result);
	ctx->traversal_active = 0;
	/* Cached entries exist only to serve singular calls made by callbacks. */
	vfsi_clear_objects(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	return rc;
}
