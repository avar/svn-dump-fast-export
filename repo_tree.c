/*
 * Licensed under a two-clause BSD-style license.
 * See LICENSE for details.
 */

#include "git-compat-util.h"

#include "string_pool.h"
#include "repo_tree.h"
#include "obj_pool.h"
#include "fast_export.h"

#include "trp.h"

struct repo_dirent {
	uint32_t name_offset;
	struct trp_node children;
	uint32_t mode;
	uint32_t content_offset;
};

struct repo_dir {
	struct trp_root entries;
};

struct repo_commit {
	uint32_t root_dir_offset;
};

/* Generate memory pools for commit, dir and dirent */
obj_pool_gen(commit, struct repo_commit, 4096);
obj_pool_gen(dir, struct repo_dir, 4096);
obj_pool_gen(dirent, struct repo_dirent, 4096);

static int repo_dirent_name_cmp(const void *a, const void *b);

/* Build a Treap from the node_s structure (a trp_node w/ offset) */
trp_gen(static, dirent_, struct repo_dirent, children, dirent, repo_dirent_name_cmp);

static uint32_t active_commit;
static uint32_t _mark;

uint32_t next_blob_mark(void)
{
	return _mark++;
}

static struct repo_dir *repo_commit_root_dir(struct repo_commit *commit)
{
	return dir_pointer(commit->root_dir_offset);
}

static struct repo_dirent *repo_first_dirent(struct repo_dir *dir)
{
	return dirent_first(&dir->entries);
}

static int repo_dirent_name_cmp(const void *a, const void *b)
{
	const struct repo_dirent *dirent1 = a, *dirent2 = b;
	uint32_t a_offset = dirent1->name_offset;
	uint32_t b_offset = dirent2->name_offset;
	return (a_offset > b_offset) - (a_offset < b_offset);
}

static int repo_dirent_is_dir(struct repo_dirent *dirent)
{
	return dirent != NULL && dirent->mode == REPO_MODE_DIR;
}

static struct repo_dir *repo_dir_from_dirent(struct repo_dirent *dirent)
{
	if (!repo_dirent_is_dir(dirent))
		return NULL;
	return dir_pointer(dirent->content_offset);
}

static struct repo_dir *repo_clone_dir(struct repo_dir *orig_dir)
{
	uint32_t orig_o, new_o;
	orig_o = dir_offset(orig_dir);
	if (orig_o >= dir_pool.committed)
		return orig_dir;
	new_o = dir_alloc(1);
	orig_dir = dir_pointer(orig_o);
	*dir_pointer(new_o) = *orig_dir;
	return dir_pointer(new_o);
}

static struct repo_dirent *repo_read_dirent(uint32_t revision, uint32_t *path)
{
	uint32_t name = 0;
	struct repo_dirent *key = dirent_pointer(dirent_alloc(1));
	struct repo_dir *dir = NULL;
	struct repo_dirent *dirent = NULL;
	dir = repo_commit_root_dir(commit_pointer(revision));
	while (~(name = *path++)) {
		key->name_offset = name;
		dirent = dirent_search(&dir->entries, key);
		if (dirent == NULL || !repo_dirent_is_dir(dirent))
			break;
		dir = repo_dir_from_dirent(dirent);
	}
	dirent_free(1);
	return dirent;
}

static void
repo_write_dirent(uint32_t *path, uint32_t mode, uint32_t content_offset,
                  uint32_t del)
{
	uint32_t name, revision, dir_o = ~0, parent_dir_o = ~0;
	struct repo_dir *dir;
	struct repo_dirent *key;
	struct repo_dirent *dirent = NULL;
	revision = active_commit;
	dir = repo_commit_root_dir(commit_pointer(revision));
	dir = repo_clone_dir(dir);
	commit_pointer(revision)->root_dir_offset = dir_offset(dir);
	while (~(name = *path++)) {
		parent_dir_o = dir_offset(dir);

		key = dirent_pointer(dirent_alloc(1));
		key->name_offset = name;

		dirent = dirent_search(&dir->entries, key);
		if (dirent == NULL)
			dirent = key;
		else
			dirent_free(1);
		
		if (dirent == key) {
			dirent->mode = REPO_MODE_DIR;
			dirent->content_offset = 0;
			dirent_insert(&dir->entries, dirent);
		}


		if (dirent_offset(dirent) < dirent_pool.committed) {
			dir_o = repo_dirent_is_dir(dirent) ? dirent->content_offset : ~0;
			dirent_remove(&dir->entries, dirent);
			dirent = dirent_pointer(dirent_alloc(1));
			dirent->name_offset = name;
			dirent->mode = REPO_MODE_DIR;
			dirent->content_offset = dir_o;
			dirent_insert(&dir->entries, dirent);
		}

		dir = repo_dir_from_dirent(dirent);
		dir = repo_clone_dir(dir);
		dirent->content_offset = dir_offset(dir);
	}
	if (dirent == NULL)
		return;
	dirent->mode = mode;
	dirent->content_offset = content_offset;
	if (del && ~parent_dir_o)
		dirent_remove(&dir_pointer(parent_dir_o)->entries, dirent);
}

uint32_t repo_copy(uint32_t revision, uint32_t *src, uint32_t *dst)
{
	uint32_t mode = 0, content_offset = 0;
	struct repo_dirent *src_dirent;
	src_dirent = repo_read_dirent(revision, src);
	if (src_dirent != NULL) {
		mode = src_dirent->mode;
		content_offset = src_dirent->content_offset;
		repo_write_dirent(dst, mode, content_offset, 0);
	}
	return mode;
}

void repo_add(uint32_t *path, uint32_t mode, uint32_t blob_mark)
{
	repo_write_dirent(path, mode, blob_mark, 0);
}

uint32_t repo_replace(uint32_t *path, uint32_t blob_mark)
{
	uint32_t mode = 0;
	struct repo_dirent *src_dirent;
	src_dirent = repo_read_dirent(active_commit, path);
	if (src_dirent != NULL) {
		mode = src_dirent->mode;
		repo_write_dirent(path, mode, blob_mark, 0);
	}
	return mode;
}

void repo_modify(uint32_t *path, uint32_t mode, uint32_t blob_mark)
{
	struct repo_dirent *src_dirent;
	src_dirent = repo_read_dirent(active_commit, path);
	if (src_dirent != NULL && blob_mark == 0) {
		blob_mark = src_dirent->content_offset;
	}
	repo_write_dirent(path, mode, blob_mark, 0);
}

void repo_delete(uint32_t *path)
{
	repo_write_dirent(path, 0, 0, 1);
}

static void repo_git_add_r(uint32_t depth, uint32_t *path, struct repo_dir *dir);

static void repo_git_add(uint32_t depth, uint32_t *path, struct repo_dirent *dirent)
{
	if (repo_dirent_is_dir(dirent)) {
		repo_git_add_r(depth, path, repo_dir_from_dirent(dirent));
	} else {
		fast_export_modify(depth, path, dirent->mode, dirent->content_offset);
	}
}

static void repo_git_add_r(uint32_t depth, uint32_t *path, struct repo_dir *dir)
{
	struct repo_dirent *de = repo_first_dirent(dir);
	while (de) {
		path[depth] = de->name_offset;
		repo_git_add(depth + 1, path, de);
		de = dirent_next(&dir->entries, de);
	}
}

static void repo_diff_r(uint32_t depth, uint32_t *path, struct repo_dir *dir1,
                        struct repo_dir *dir2)
{
	struct repo_dirent *de1, *de2;
	de1 = repo_first_dirent(dir1);
	de2 = repo_first_dirent(dir2);

	while (de1 && de2) {
		if (de1->name_offset < de2->name_offset) {
			path[depth] = de1->name_offset;
			fast_export_delete(depth + 1, path);
			de1 = dirent_next(&dir1->entries, de1);
			continue;
		} else if (de1->name_offset > de2->name_offset) {
			path[depth] = de2->name_offset;
			repo_git_add(depth + 1, path, de2);
			de2 = dirent_next(&dir2->entries, de2);
			continue;
		}
		path[depth] = de1->name_offset;
		if (de1->mode != de2->mode ||
		    de1->content_offset != de2->content_offset) {
			if (repo_dirent_is_dir(de1) && repo_dirent_is_dir(de2)) {
				repo_diff_r(depth + 1, path,
				            repo_dir_from_dirent(de1),
				            repo_dir_from_dirent(de2));
			} else {
				if (repo_dirent_is_dir(de1) != repo_dirent_is_dir(de2)) {
					fast_export_delete(depth + 1, path);
				}
				repo_git_add(depth + 1, path, de2);
			}
		}
		de1 = dirent_next(&dir1->entries, de1);
		de2 = dirent_next(&dir2->entries, de2);
	}
	while (de1) {
		path[depth] = de1->name_offset;
		fast_export_delete(depth + 1, path);
		de1 = dirent_next(&dir1->entries, de1);
	}
	while (de2) {
		path[depth] = de2->name_offset;
		repo_git_add(depth + 1, path, de2);
		de2 = dirent_next(&dir2->entries, de2);
	}
}

static uint32_t path_stack[REPO_MAX_PATH_DEPTH];

void repo_diff(uint32_t r1, uint32_t r2)
{
	repo_diff_r(0,
	            path_stack,
	            repo_commit_root_dir(commit_pointer(r1)),
	            repo_commit_root_dir(commit_pointer(r2)));
}

void repo_commit(uint32_t revision, uint32_t author, char *log, uint32_t uuid,
                 uint32_t url, unsigned long timestamp)
{
	fast_export_commit(revision, author, log, uuid, url, timestamp);
	pool_commit();
	dirent_commit();
	dir_commit();
	commit_commit();
	active_commit = commit_alloc(1);
	commit_pointer(active_commit)->root_dir_offset =
		commit_pointer(active_commit - 1)->root_dir_offset;
}

static void mark_init(void)
{
	uint32_t i;
	_mark = 0;
	for (i = 0; i < dirent_pool.size; i++)
		if (!repo_dirent_is_dir(dirent_pointer(i)) &&
		    dirent_pointer(i)->content_offset > _mark)
			_mark = dirent_pointer(i)->content_offset;
	_mark++;
}

void repo_init() {
	pool_init();
	commit_init();
	dir_init();
	dirent_init();
	mark_init();
	if (commit_pool.size == 0) {
		/* Create empty tree for commit 0. */
		commit_alloc(1);
		commit_pointer(0)->root_dir_offset = dir_alloc(1);
		dir_pointer(0)->entries.trp_root = ~0;
		dir_commit();
		commit_commit();
	}
	/* Preallocate next commit, ready for changes. */
	active_commit = commit_alloc(1);
	commit_pointer(active_commit)->root_dir_offset =
		commit_pointer(active_commit - 1)->root_dir_offset;
}

void repo_reset(void)
{
	pool_reset();
	commit_reset();
	dir_reset();
	dirent_reset();
}
