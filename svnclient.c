#include "cache.h"
#include "remote.h"
#include "svndump.h"

int open_svn_connection(const char *url)
{
    /* TODO: Open coennection to remote */
    buffer_init(url);
    return 0;
}

int close_svn_connection()
{
    /* TODO: Close connection */
    buffer_deinit();
    return 0;
}

int FI_svn (char *spec)
{
    svndump_read(NULL);
    svndump_reset();
    return 0;
}

int FE_svn ()
{
    /* TODO: Write a fast-import module for SVN to import from the
       Git fast-export stream */
    return 0;
}
