/*
 * Licensed under a two-clause BSD-style license.
 * See LICENSE for details.
 */

#ifndef OBJ_POOL_H_
#define OBJ_POOL_H_

#include "git-compat-util.h"

/*
 * The obj_pool_gen() macro generates a type-specific memory pool
 * implementation.
 *
 * Arguments:
 *
 *   pre              : Prefix for generated functions (ex: string_).
 *   obj_t            : Type for treap data structure (ex: char).
 *   intial_capacity  : The initial size of the memory pool (ex: 4096).
 *
 */
#define obj_pool_gen(pre, obj_t, initial_capacity) \
static struct { \
	uint32_t committed; \
	uint32_t size; \
	uint32_t capacity; \
	obj_t *base; \
	FILE *file; \
} pre##_pool = { 0, 0, 0, NULL, NULL}; \
static void pre##_init(void) \
{ \
	struct stat st; \
	pre##_pool.file = fopen(#pre ".bin", "a+"); \
	rewind(pre##_pool.file); \
	fstat(fileno(pre##_pool.file), &st); \
	pre##_pool.size = st.st_size / sizeof(obj_t); \
	pre##_pool.committed = pre##_pool.size; \
	pre##_pool.capacity = pre##_pool.size * 2; \
	if (pre##_pool.capacity < initial_capacity) \
		pre##_pool.capacity = initial_capacity; \
	pre##_pool.base = malloc(pre##_pool.capacity * sizeof(obj_t)); \
	fread(pre##_pool.base, sizeof(obj_t), pre##_pool.size, pre##_pool.file); \
} \
static uint32_t pre##_alloc(uint32_t count) \
{ \
	uint32_t offset; \
	if (pre##_pool.size + count > pre##_pool.capacity) { \
		while (pre##_pool.size + count > pre##_pool.capacity) \
			if (pre##_pool.capacity) \
				pre##_pool.capacity *= 2; \
			else \
				pre##_pool.capacity = initial_capacity; \
		pre##_pool.base = realloc(pre##_pool.base, \
					pre##_pool.capacity * sizeof(obj_t)); \
	} \
	offset = pre##_pool.size; \
	pre##_pool.size += count; \
	return offset; \
} \
static void pre##_free(uint32_t count) \
{ \
	pre##_pool.size -= count; \
} \
static uint32_t pre##_offset(obj_t *obj) \
{ \
	return obj == NULL ? ~0 : obj - pre##_pool.base; \
} \
static obj_t *pre##_pointer(uint32_t offset) \
{ \
	return offset >= pre##_pool.size ? NULL : &pre##_pool.base[offset]; \
} \
static void pre##_commit(void) \
{ \
	pre##_pool.committed += fwrite(pre##_pool.base + pre##_pool.committed, \
		sizeof(obj_t), pre##_pool.size - pre##_pool.committed, \
		pre##_pool.file); \
} \
static void pre##_reset(void) \
{ \
	if (pre##_pool.base) { \
		free(pre##_pool.base); \
		fclose(pre##_pool.file); \
	} \
	pre##_pool.base = NULL; \
	pre##_pool.size = 0; \
	pre##_pool.capacity = 0; \
	pre##_pool.file = NULL; \
}

#endif
