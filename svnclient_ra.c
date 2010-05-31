#include "svn_pools.h"
#include "svn_cmdline.h"
#include "svn_client.h"
#include "svn_ra.h"

#include "delta_editor.h"

static apr_pool_t *pool = NULL;
static svn_client_ctx_t *ctx = NULL;
static svn_ra_session_t *session = NULL;

static svn_error_t *setup_delta_editor(svn_delta_editor_t **editor)
{
	*editor = svn_delta_default_editor(pool);
	(*editor)->set_target_revision = set_target_revision;
	(*editor)->open_root = open_root;
	(*editor)->delete_entry = delete_entry;
	(*editor)->add_directory = add_directory;
	(*editor)->open_directory = open_directory;
	(*editor)->add_file = add_file;
	(*editor)->open_file = open_file;
	(*editor)->apply_textdelta = apply_textdelta;
	(*editor)->close_file = close_file;
	(*editor)->close_directory = close_directory;
	(*editor)->change_file_prop = change_file_prop;
	(*editor)->change_dir_prop = change_dir_prop;
	(*editor)->close_edit = close_edit;
	(*editor)->absent_directory = absent_directory;
	(*editor)->absent_file = absent_file;
	(*editor)->abort_edit = abort_edit;
	return SVN_NO_ERROR;
}

static svn_error_t *replay_revstart(svn_revnum_t revision,
                                    void *replay_baton,
                                    const svn_delta_editor_t **editor,
                                    void **edit_baton,
                                    apr_hash_t *rev_props,
                                    apr_pool_t *pool)
{
	return SVN_NO_ERROR;
}

static svn_error_t *replay_revend(svn_revnum_t revision,
                                  void *replay_baton,
                                  const svn_delta_editor_t *editor,
                                  void *edit_baton,
                                  apr_hash_t *rev_props,
                                  apr_pool_t *pool)
{
	SVN_ERR(editor->close_edit(edit_baton, pool));
	return SVN_NO_ERROR;
}

svn_error_t *build_auth_baton()
{
	svn_auth_provider_object_t *provider;
	apr_array_header_t *providers
		= apr_array_make (pool, 4, sizeof (svn_auth_provider_object_t *));

	svn_auth_get_simple_prompt_provider (&provider,
	                                     NULL,
	                                     NULL,
	                                     2,
	                                     pool);
	APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

	svn_auth_get_username_prompt_provider (&provider,
	                                       NULL,
	                                       NULL,
	                                       2,
	                                       pool);
	APR_ARRAY_PUSH (providers, svn_auth_provider_object_t *) = provider;

	svn_auth_open (&ctx->auth_baton, providers, pool);
	return SVN_NO_ERROR;
}

svn_error_t *populate_context()
{
	SVN_ERR(svn_config_get_config(&(ctx->config), NULL, pool));
	return SVN_NO_ERROR;
}

svn_error_t *open_connection(const char *url)
{
	SVN_ERR(svn_config_ensure (NULL, pool));
	SVN_ERR(svn_client_create_context (&ctx, pool));
	SVN_ERR(svn_ra_initialize(pool));

#if defined(WIN32) || defined(__CYGWIN__)
	if (getenv("SVN_ASP_DOT_NET_HACK"))
		SVN_ERR(svn_wc_set_adm_dir("_svn", pool));
#endif
	
	SVN_ERR(populate_context());
	SVN_ERR(build_auth_baton());
	SVN_ERR(svn_client_open_ra_session(&session, url, ctx, pool));
	return SVN_NO_ERROR;
}

svn_error_t *replay_range(svn_revnum_t start_revision, svn_revnum_t end_revision)
{
	svn_revnum_t latest_revision;
	svn_delta_editor_t *editor;
	svn_error_t *err;
	SVN_ERR(svn_ra_get_latest_revnum(session, &latest_revision, pool));
	printf("%ld\n", latest_revision);
	SVN_ERR(setup_delta_editor(&editor));
	SVN_ERR(svn_ra_replay_range(session, start_revision, end_revision,
	                            0, TRUE, replay_revstart, replay_revend, NULL, pool));
	return SVN_NO_ERROR;
}

void close_connection()
{
	svn_pool_destroy(pool);
}

int main()
{
	const char url[] = "http://svn.apache.org/repos/asf/subversion/trunk";
	svn_revnum_t start_revision = 0, end_revision = 5;
	if (svn_cmdline_init ("svnclient_ra", stderr) != EXIT_SUCCESS)
		return EXIT_FAILURE;
	pool = svn_pool_create(NULL);

	if(open_connection(url) != SVN_NO_ERROR)
		return 1;
	if(replay_range(start_revision, end_revision) != SVN_NO_ERROR)
		return 1;

	close_connection();
	return 0;
}
