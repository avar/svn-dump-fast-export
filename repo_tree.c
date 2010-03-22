/******************************************************************************
 *
 * Copyright (C) 2010 David Barr <david.barr@cordelta.com>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice(s), this list of conditions and the following disclaimer
 *    unmodified other than the allowable addition of one or more
 *    copyright notices.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice(s), this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ******************************************************************************/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "string_pool.h"
#include "repo_tree.h"
#include "obj_pool.h"

obj_pool_gen(commit, repo_commit_t, 4096);
obj_pool_gen(dir, repo_dir_t, 4096);
obj_pool_gen(gc_dir, repo_dir_gc_t, 4096);
obj_pool_gen(dirent, repo_dirent_t, 4096);

static uint32_t num_dirs_saved = 0;
static uint32_t num_dirents_saved = 0;
static uint32_t active_commit = -1;

static repo_dir_t *repo_commit_root_dir(repo_commit_t * commit)
{
    return dir_pointer(commit->root_dir_offset);
}

static repo_dirent_t *repo_first_dirent(repo_dir_t * dir)
{
    return dirent_pointer(dir->first_offset);
}

static int repo_dirent_name_cmp(const void *a, const void *b)
{
    return (((repo_dirent_t *) a)->name_offset
            > ((repo_dirent_t *) b)->name_offset) -
        (((repo_dirent_t *) a)->name_offset
         < ((repo_dirent_t *) b)->name_offset);
}

static repo_dirent_t *repo_dirent_by_name(repo_dir_t * dir,
                                          uint32_t name_offset)
{
    repo_dirent_t key;
    if (dir->size == 0)
        return NULL;
    key.name_offset = name_offset;
    return bsearch(&key, repo_first_dirent(dir), dir->size,
                   sizeof(repo_dirent_t), repo_dirent_name_cmp);
}

static int repo_dirent_is_dir(repo_dirent_t * dirent)
{
    return dirent->mode == REPO_MODE_DIR;
}

static int repo_dirent_is_blob(repo_dirent_t * dirent)
{
    return dirent->mode == REPO_MODE_BLB || dirent->mode == REPO_MODE_EXE;
}

static int repo_dirent_is_executable(repo_dirent_t * dirent)
{
    return dirent->mode == REPO_MODE_EXE;
}

static int repo_dirent_is_symlink(repo_dirent_t * dirent)
{
    return dirent->mode == REPO_MODE_LNK;
}

static repo_dir_t *repo_dir_from_dirent(repo_dirent_t * dirent)
{
    if (!repo_dirent_is_dir(dirent))
        return NULL;
    return dir_pointer(dirent->content_offset);
}

static uint32_t dir_with_dirents_alloc(uint32_t size)
{
    uint32_t offset = dir_alloc(1);
    dir_pointer(offset)->size = size;
    dir_pointer(offset)->first_offset = dirent_alloc(size);
    return offset;
}

static repo_dir_t *repo_clone_dir(repo_dir_t * orig_dir, uint32_t padding)
{
    uint32_t orig_o, new_o, dirent_o;
    orig_o = dir_offset(orig_dir);
    if (orig_o < num_dirs_saved) {
        new_o = dir_with_dirents_alloc(orig_dir->size + padding);
        orig_dir = dir_pointer(orig_o);
        dirent_o = dir_pointer(new_o)->first_offset;
    } else {
        if (padding == 0)
            return orig_dir;
        new_o = orig_o;
        dirent_o = dirent_alloc(orig_dir->size + padding);
    }
    memcpy(dirent_pointer(dirent_o), repo_first_dirent(orig_dir),
           orig_dir->size * sizeof(repo_dirent_t));
    if (orig_o >= num_dirs_saved) {
        bzero(repo_first_dirent(orig_dir),
              orig_dir->size * sizeof(repo_dirent_t));
    }
    bzero(dirent_pointer(dirent_o + orig_dir->size),
          padding * sizeof(repo_dirent_t));
    dir_pointer(new_o)->size = orig_dir->size + padding;
    dir_pointer(new_o)->first_offset = dirent_o;
    return dir_pointer(new_o);
}

static repo_dirent_t *repo_read_dirent(uint32_t revision, char *path)
{
    char *ctx = NULL;
    uint32_t name = 0;
    repo_dir_t *dir = NULL;
    repo_dirent_t *dirent = NULL;
    dir = repo_commit_root_dir(commit_pointer(revision));
    for (name = pool_tok_r(path, "/", &ctx);
         name; name = pool_tok_r(NULL, "/", &ctx)) {
        dirent = repo_dirent_by_name(dir, name);
        if (dirent == NULL) {
            return NULL;
        } else if (repo_dirent_is_dir(dirent)) {
            dir = repo_dir_from_dirent(dirent);
        } else {
            break;
        }
    }
    return dirent;
}

static void
repo_write_dirent(char *path, uint32_t mode, uint32_t content_offset,
                  uint32_t del)
{
    char *ctx, *end;
    uint32_t name, revision, dirent_o, dir_o, parent_dir_o;
    repo_dir_t *dir;
    repo_dirent_t *dirent = NULL;
    end = path + strlen(path);
    revision = active_commit;
    dir = repo_commit_root_dir(commit_pointer(revision));
    dir = repo_clone_dir(dir, 0);
    commit_pointer(revision)->root_dir_offset = dir_offset(dir);
    for (name = pool_tok_r(path, "/", &ctx); name;
         name = pool_tok_r(NULL, "/", &ctx)) {
        parent_dir_o = dir_offset(dir);
        dirent = repo_dirent_by_name(dir, name);
        if (dirent == NULL) {
            dir = repo_clone_dir(dir, 1);
            dirent = &repo_first_dirent(dir)[dir->size - 1];
            dirent->name_offset = name;
            dirent->mode = REPO_MODE_DIR;
            qsort(repo_first_dirent(dir), dir->size,
                  sizeof(repo_dirent_t), repo_dirent_name_cmp);
            dirent = repo_dirent_by_name(dir, name);
            dir_o = dir_with_dirents_alloc(0);
            dirent->content_offset = dir_o;
            dir = dir_pointer(dir_o);
        } else if (dir = repo_dir_from_dirent(dirent)) {
            dirent_o = dirent_offset(dirent);
            dir = repo_clone_dir(dir, 0);
            if (dirent_o != ~0)
                dirent_pointer(dirent_o)->content_offset = dir_offset(dir);
        } else {
            dirent->mode = REPO_MODE_DIR;
            dirent_o = dirent_offset(dirent);
            dir_o = dir_with_dirents_alloc(0);
            dirent = dirent_pointer(dirent_o);
            dir = dir_pointer(dir_o);
            dirent->content_offset = dir_o;
        }
    }
    if (del)
        dirent->name_offset = ~0;
    dirent->mode = mode;
    dirent->content_offset = content_offset;
    if (del) {
        dir = dir_pointer(parent_dir_o);
        qsort(repo_first_dirent(dir), dir->size,
              sizeof(repo_dirent_t), repo_dirent_name_cmp);
        dir->size--;
    }
}

void repo_copy(uint32_t revision, char *src, char *dst)
{
    repo_dirent_t *src_dirent;
    fprintf(stderr, "C %d:%s %s\n", revision, src, dst);
    src_dirent = repo_read_dirent(revision, src);
    if (src_dirent == NULL)
        return;
    repo_write_dirent(dst, src_dirent->mode, src_dirent->content_offset,
                      0);
}

void repo_add(char *path, uint32_t mode, uint32_t blob_mark)
{
    fprintf(stderr, "A %s %d\n", path, blob_mark);
    repo_write_dirent(path, mode, blob_mark, 0);
}

void repo_modify(char *path, uint32_t blob_mark)
{
    fprintf(stderr, "M %s %d\n", path, blob_mark);
    repo_write_dirent(path, REPO_MODE_BLB, blob_mark, 0);
}

void repo_delete(char *path)
{
    fprintf(stderr, "D %s\n", path);
    repo_write_dirent(path, 0, 0, 1);
}

static void repo_gc_mark_dirs(repo_dir_t * dir)
{
    uint32_t i, j, offset;
    repo_dirent_t *dirent;
    if (dir->size) {
        offset = gc_dir_alloc(1);
        gc_dir_pointer(offset)->offset = dir_offset(dir);
        gc_dir_pointer(offset)->dir = *dir;
    }
    for (j = 0; j < dir->size; j++) {
        dirent = &repo_first_dirent(dir)[j];
        if (repo_dirent_is_dir(dirent) &&
            dirent->content_offset >= num_dirs_saved) {
            repo_gc_mark_dirs(repo_dir_from_dirent(dirent));
        }
    }
}

static int repo_gc_dir_offset_cmp(const void *a, const void *b)
{
    return (((repo_dir_gc_t *) a)->dir.first_offset
            > ((repo_dir_gc_t *) b)->dir.first_offset) -
        (((repo_dir_gc_t *) a)->dir.first_offset
         < ((repo_dir_gc_t *) b)->dir.first_offset);
}

static int repo_gc_dir_src_cmp(const void *a, const void *b)
{
    return (((repo_dir_gc_t *) a)->offset
            > ((repo_dir_gc_t *) b)->offset) -
        (((repo_dir_gc_t *) a)->offset < ((repo_dir_gc_t *) b)->offset);
}

static repo_dir_gc_t *repo_gc_find_by_src(uint32_t offset)
{
    uint32_t i;
    for (i = 0; i < gc_dir_pool.size; i++) {
        if (gc_dir_pointer(i)->offset == offset) {
            return gc_dir_pointer(i);
        }
    }
    return NULL;
}

static void repo_gc_dirents(void)
{
    repo_dir_gc_t *gc_dir;
    uint32_t i;
    uint32_t offset = num_dirents_saved;
    for (i = 0; i < gc_dir_pool.size; i++) {
        gc_dir = gc_dir_pointer(i);
        memmove(dirent_pointer(offset),
                dirent_pointer(gc_dir->dir.first_offset),
                gc_dir->dir.size * sizeof(repo_dirent_t));
        gc_dir->dir.first_offset = offset;
        offset += gc_dir->dir.size;
    }
    dirent_pool.size = offset;
}

static void repo_gc_dirs(void)
{
    uint32_t i;
    repo_dir_gc_t *gc_dir;
    repo_commit_t *commit = commit_pointer(active_commit);
    repo_dir_t *root = repo_commit_root_dir(commit);
    gc_dir_pool.size = 0;
    repo_gc_mark_dirs(root);
    qsort(gc_dir_pointer(0), gc_dir_pool.size, sizeof(repo_dir_gc_t),
          repo_gc_dir_offset_cmp);
    repo_gc_dirents();
    gc_dir = repo_gc_find_by_src(commit->root_dir_offset);
    commit->root_dir_offset =
        gc_dir == NULL ? 0 : (gc_dir_offset(gc_dir) + num_dirs_saved);
    for (i = num_dirents_saved; i < dirent_pool.size; i++) {
        if (repo_dirent_is_dir(dirent_pointer(i)) &&
            dirent_pointer(i)->content_offset >= num_dirs_saved) {
            gc_dir =
                repo_gc_find_by_src(dirent_pointer(i)->content_offset);
            if (gc_dir) {
                dirent_pointer(i)->content_offset =
                    gc_dir_offset(gc_dir) + num_dirs_saved;
            } else {
                dirent_pointer(i)->content_offset = 0;
            }
        }
    }
    for (i = 0; i < gc_dir_pool.size; i++) {
        *dir_pointer(num_dirs_saved + i) = gc_dir_pointer(i)->dir;
    }
    dir_pool.size = num_dirs_saved + gc_dir_pool.size;
}

void repo_commit(uint32_t revision)
{
    fprintf(stderr, "R %d\n", revision);
    if (revision == 0) {
        active_commit = commit_alloc(1);
        commit_pointer(active_commit)->root_dir_offset =
            dir_with_dirents_alloc(0);
    } else {
        repo_gc_dirs();
    }
    num_dirs_saved = dir_pool.size;
    num_dirents_saved = dirent_pool.size;
    active_commit = commit_alloc(1);
    commit_pointer(active_commit)->root_dir_offset =
        commit_pointer(active_commit - 1)->root_dir_offset;
}

static void repo_print_path(uint32_t depth, uint32_t * path)
{
    uint32_t p;
    for (p = 0; p < depth; p++) {
        fputs(pool_fetch(path[p]), stdout);
        if (p < depth - 1)
            putchar('/');
    }
}

static void repo_git_delete(uint32_t depth, uint32_t * path)
{
    putchar('D');
    putchar(' ');
    repo_print_path(depth, path);
    putchar('\n');
}

static void
repo_git_add_r(uint32_t depth, uint32_t * path, repo_dir_t * dir);

static void
repo_git_add(uint32_t depth, uint32_t * path, repo_dirent_t * dirent)
{
    if (repo_dirent_is_dir(dirent)) {
        repo_git_add_r(depth, path, repo_dir_from_dirent(dirent));
    } else {
        printf("M %06o :%d ", dirent->mode, dirent->content_offset);
        repo_print_path(depth, path);
        putchar('\n');
    }
}

static void
repo_git_add_r(uint32_t depth, uint32_t * path, repo_dir_t * dir)
{
    uint32_t o;
    repo_dirent_t *de;
    de = repo_first_dirent(dir);
    for (o = 0; o < dir->size; o++) {
        path[depth] = de[o].name_offset;
        repo_git_add(depth + 1, path, &de[o]);
    }
}

static void
repo_diff_r(uint32_t depth, uint32_t * path, repo_dir_t * dir1,
            repo_dir_t * dir2)
{
    uint32_t o1, o2, p;
    repo_dirent_t *de1, *de2;
    de1 = repo_first_dirent(dir1);
    de2 = repo_first_dirent(dir2);
    for (o1 = o2 = 0; o1 < dir1->size && o2 < dir2->size;) {
        if (de1[o1].name_offset < de2[o2].name_offset) {
            /* delete(o1) */
            path[depth] = de1[o1].name_offset;
            repo_git_delete(depth + 1, path);
            o1++;
        } else if (de1[o1].name_offset == de2[o2].name_offset) {
            path[depth] = de1[o1].name_offset;
            if (de1[o1].content_offset != de2[o2].content_offset) {
                if (repo_dirent_is_dir(&de1[o1])
                    && repo_dirent_is_dir(&de2[o2])) {
                    /* recursive diff */
                    repo_diff_r(depth + 1, path,
                                repo_dir_from_dirent(&de1[o1]),
                                repo_dir_from_dirent(&de2[o2]));
                } else {
                    /* delete o1, add o2 */
                    if (repo_dirent_is_dir(&de1[o1]) !=
                        repo_dirent_is_dir(&de2[o2])) {
                        repo_git_delete(depth + 1, path);
                    }
                    repo_git_add(depth + 1, path, &de2[o2]);
                }
            }
            o1++;
            o2++;
        } else {
            /* add(o2) */
            path[depth] = de2[o2].name_offset;
            repo_git_add(depth + 1, path, &de2[o2]);
            o2++;
        }
    }
    for (; o1 < dir1->size; o1++) {
        /* delete(o1) */
        path[depth] = de1[o1].name_offset;
        repo_git_delete(depth + 1, path);
    }
    for (; o2 < dir2->size; o2++) {
        /* add(o2) */
        path[depth] = de2[o2].name_offset;
        repo_git_add(depth + 1, path, &de2[o2]);
    }
}

void repo_diff(uint32_t r1, uint32_t r2)
{
    uint32_t path_stack[1000];
    repo_diff_r(0,
                path_stack,
                repo_commit_root_dir(commit_pointer(r1)),
                repo_commit_root_dir(commit_pointer(r2)));
}
