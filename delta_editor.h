#ifndef DELTA_EDITOR_H_
#define DELTA_EDITOR_H_

#include "svn_pools.h"
#include "svn_cmdline.h"
#include "svn_client.h"
#include "svn_ra.h"

svn_error_t *set_target_revision(void *edit_baton,
                                 svn_revnum_t target_revision,
                                 apr_pool_t *pool);

svn_error_t *open_root(void *edit_baton, svn_revnum_t base_revision,
                       apr_pool_t *dir_pool, void **root_baton);

svn_error_t *delete_entry(const char *path, svn_revnum_t revision,
                          void *parent_baton, apr_pool_t *pool);

svn_error_t *add_directory(const char *path, void *parent_baton,
                           const char *copyfrom_path,
                           svn_revnum_t copyfrom_revision,
                           apr_pool_t *dir_pool, void **child_baton);


svn_error_t *open_directory(const char *path, void *parent_baton,
                            svn_revnum_t base_revision,
                            apr_pool_t *dir_pool, void **child_baton);

svn_error_t *change_dir_prop(void *dir_baton, const char *name,
                             const svn_string_t *value, apr_pool_t *pool);

svn_error_t *close_directory(void *dir_baton, apr_pool_t *pool);

svn_error_t *absent_directory(const char *path, void *parent_baton,
                              apr_pool_t *pool);

svn_error_t *add_file(const char *path, void *parent_baton,
                      const char *copyfrom_path,
                      svn_revnum_t copyfrom_revision,
                      apr_pool_t *file_pool, void **file_baton);

svn_error_t *open_file(const char *path, void *parent_baton,
                       svn_revnum_t base_revision, apr_pool_t *file_pool,
                       void **file_baton);

svn_error_t *apply_textdelta(void *file_baton,
                             const char *base_checksum,
                             apr_pool_t *pool,
                             svn_txdelta_window_handler_t *handler,
                             void **handler_baton);

svn_error_t *change_file_prop(void *file_baton, const char *name,
                              const svn_string_t *value,
                              apr_pool_t *pool);

svn_error_t *close_file(void *file_baton, const char *text_checksum,
                        apr_pool_t *pool);

svn_error_t *absent_file(const char *path, void *parent_baton,
                         apr_pool_t *pool);

svn_error_t *close_edit(void *edit_baton, apr_pool_t *pool);

svn_error_t *abort_edit(void *edit_baton, apr_pool_t *pool);

#endif
