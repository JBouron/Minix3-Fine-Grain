#include <sys/cdefs.h>
#include "namespace.h"
#include <lib.h>

#include <string.h>
#include <unistd.h>

int
close(int fd)
{
	message m;

	memset(&m, 0, sizeof(m));
	m.m_lc_vfs_close.fd = fd;

	// Make the request blocking to avoid socket bug.
	m.m_lc_vfs_close.nblock = 1;

	return _syscall(VFS_PROC_NR, VFS_CLOSE, &m);
}
