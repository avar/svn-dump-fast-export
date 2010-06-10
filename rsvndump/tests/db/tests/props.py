#
#	Test database for rsvndump
#	written by Jonas Gehring
#


import os

import test_api


def info():
	return "Property test"


def setup(step, log):
	if step == 0:
		os.mkdir("dir1")
		f = open("dir1/file1","wb")
		print >>f, "hello1"
		test_api.run("svn", "add", "dir1", output = log)
		return True
	elif step == 1:
		test_api.run("svn", "propset", "copyright", "(c) ME", "dir1/file1", output = log)
		test_api.run("svn", "propset", "license", "public domain", "dir1/file1", output = log)
		test_api.run("svn", "propset", "bla", "blubb", "dir1/file1", output = log)
		return True
	elif step == 2:
		test_api.run("svn", "up", output = log)
		test_api.run("svn", "propset", "svn:ignore", "*.o", "dir1", output = log)
		return True
	elif step == 3:
		os.mkdir("dir2")
		test_api.run("svn", "add", "dir2", output = log)
		test_api.run("svn", "propset", "svn:externals", "ext/test    http://svn.collab.net/repos/svn/trunk/", "dir2", output = log)
		return True
	elif step == 4:
		os.mkdir("dir3")
		os.mkdir("dir3/subdir")
		f = open("dir3/subdir/file1","wb")
		print >>f, "hello1"
		test_api.run("svn", "add", "dir3", output = log)
		test_api.run("svn", "propset", "svn:externals", "ext/subversion    http://svn.collab.net/repos/svn/trunk/", "dir3", output = log)
		return True
	elif step == 5:
		test_api.run("svn", "propset", "svn:eol-style", "native", "dir1/file1", output = log)
		return True
	elif step == 6:
		test_api.run("svn", "up", "--ignore-externals", output = log)
		test_api.run("svn", "propdel", "svn:ignore", "dir1", output = log)
		return True
	elif step == 7:
		f = open("dir1/file1","wb")
		print >>f, "hello2"
		test_api.run("svn", "propset", "svn:eol-style", "LF", "dir1/file1", output = log)
		return True
	elif step == 8:
		test_api.run("svn", "propdel", "copyright", "dir1/file1", output = log)
		return True
#	elif step == 9:
#		test_api.run("svn", "cp", "dir3", "dir4", output = log)
#		return True
#	elif step == 10:
#		test_api.run("svn", "up", output = log)
#		test_api.run("svn", "mv", "dir1", "dir4", output = log)
#		test_api.run("svn", "propset", "svn:ignore", "*.[oa]", "dir4", output = log)
#		return True
#	elif step == 11:
#		test_api.run("svn", "up", output = log)
#		test_api.run("svn", "mv", "dir4/file1", "dir4/file2", output = log)
#		return True
	else:
		return False


# Runs the test
def run(id, args = []):
	# Set up the test repository
	test_api.setup_repos(id, setup)

	odump_path = test_api.dump_original(id)
	rdump_path = test_api.dump_rsvndump(id, args)
	vdump_path = test_api.dump_reload(id, rdump_path)

	return test_api.diff(id, odump_path, vdump_path)
