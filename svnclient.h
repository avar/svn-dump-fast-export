#ifndef SVNCLIENT_H
#define SVNCLIENT_H

int open_svn_connection(const char *url);
int close_svn_connection();
int FI_svn (char *spec);
int FE_svn ();

#endif
