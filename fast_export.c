#include <string.h>

#include "fast_export.h"
#include "line_buffer.h"
#include "repo_tree.h"
#include "string_pool.h"

#define MAX_GITSVN_LINE_LEN 4096

void fast_export_delete(uint32_t depth, uint32_t * path)
{
    putchar('D');
    putchar(' ');
    pool_print_seq(depth, path, '/', stdout);
    putchar('\n');
}

void fast_export_modify(uint32_t depth, uint32_t * path, uint32_t mode,
                        uint32_t mark)
{
    printf("M %06o :%d ", mode, mark);
    pool_print_seq(depth, path, '/', stdout);
    putchar('\n');
}

static char gitsvnline[MAX_GITSVN_LINE_LEN];
void fast_export_commit(uint32_t revision, char * author, char * log,
                        char * uuid, char * url, time_t timestamp)
{
    printf("commit refs/heads/master\nmark :%d\n", revision);
    printf("committer %s <%s@%s> %ld +0000\n",
         author, author, uuid ? uuid : "local", timestamp);
    if (uuid && url) {
        snprintf(gitsvnline, MAX_GITSVN_LINE_LEN, "\n\ngit-svn-id: %s@%d %s\n",
             url, revision, uuid);
    } else {
        *gitsvnline = '\0';
    }
    printf("data %ld\n%s%s\n",
           strlen(log) + strlen(gitsvnline), log, gitsvnline);
    repo_diff(revision - 1, revision);
    fputc('\n', stdout);

    printf("progress Imported commit %d.\n\n", revision);
}

void fast_export_blob(uint32_t mode, uint32_t mark, uint32_t len)
{
    if (mode == REPO_MODE_LNK) {
        /* svn symlink blobs start with "link " */
        buffer_skip_bytes(5);
        len -= 5;
    }
    printf("blob\nmark :%d\ndata %d\n", mark, len);
    buffer_copy_bytes(len);
    fputc('\n', stdout);
}
