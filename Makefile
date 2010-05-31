svnclient_ra: svnclient_ra.c
	cc -Wall -Werror -ggdb -O1 -o $@ -lsvn_client-1 svnclient_ra.c delta_editor.c -I. -I/usr/include/subversion-1 -I/usr/include/apr-1.0
