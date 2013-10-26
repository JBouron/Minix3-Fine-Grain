#define NCALLS		 125	/* number of system calls allowed */

/* In case it isn't obvious enough: this list is sorted numerically. */
#define EXIT		   1 
#define FORK		   2 
#define READ		   3 
#define WRITE		   4 
#define OPEN		   5 
#define CLOSE		   6 
#define WAIT		   7
#define LINK		   9 
#define UNLINK		  10 
#define WAITPID		  11
#define CHDIR		  12 
#define TIME		  13
#define MKNOD		  14 
#define CHMOD		  15 
#define CHOWN		  16 
#define BRK		  17
#define MINIX_GETPID	  20
#define MOUNT		  21 
#define UMOUNT		  22 
#define SETUID		  23
#define GETUID		  24
#define STIME		  25
#define PTRACE		  26
#define UTIME		  30 
#define GETEPINFO	  31
#define SETGROUPS	  32
#define ACCESS		  33 
#define GETGROUPS	  34
#define SYNC		  36 
#define KILL		  37
#define RENAME		  38
#define MKDIR		  39
#define RMDIR		  40
#define PIPE		  42 
#define SYMLINK		  45
#define SETGID		  46
#define GETGID		  47
#define SIGNAL		  48
#define RDLNK		  49
#define STAT		  51
#define FSTAT		  52
#define LSTAT		  53
#define IOCTL		  54
#define COPYFD		  56
#define FS_READY	  57
#define PIPE2		  58
#define EXEC		  59
#define UMASK		  60 
#define CHROOT		  61 
#define SETSID		  62
#define GETPGRP		  63
#define ITIMER		  64
#define GETMCONTEXT       67
#define SETMCONTEXT       68
#define GETDENTS	  69
#define FTRUNCATE	  70

/* Posix signal handling. */
#define SIGACTION	  71
#define SIGSUSPEND	  72
#define SIGPENDING	  73
#define SIGPROCMASK	  74
#define SIGRETURN	  75

#define REBOOT		  76
#define SVRCTL		  77
#define SYSUNAME	  78
#define LLSEEK		  81	/* to VFS */
#define GETVFSSTAT	  82	/* to VFS */
#define STATVFS1 	  83	/* to VFS */
#define FSTATVFS1 	  84	/* to VFS */
#define SELECT            85	/* to VFS */
#define FCHDIR            86	/* to VFS */
#define FSYNC             87	/* to VFS */
#define GETPRIORITY       88	/* to PM */
#define SETPRIORITY       89	/* to PM */
#define GETTIMEOFDAY      90	/* to PM */
#define SETEUID		  91	/* to PM */
#define SETEGID		  92	/* to PM */
#define FCHMOD		  95	/* to VFS */
#define FCHOWN		  96	/* to VFS */
#define LSEEK		  97
#define SPROF             98    /* to PM */
#define CPROF             99    /* to PM */

#define PM_NEWEXEC	100	/* from VFS or RS to PM: new exec */
#define SRV_FORK  	101	/* to PM: special fork call for RS */
#define EXEC_RESTART	102	/* to PM: final part of exec for RS */
#define GETPROCNR	104	/* to PM */
#define ISSETUGID	106	/* to PM: ask if process is tainted */
#define GETEPINFO_O	107	/* to PM: get pid/uid/gid of an endpoint */
#define UTIMENS		108	/* to FS: [f]utimens(); also [fl]utimes */
#define FCNTL		109	/* to VFS */
#define TRUNCATE	110	/* to VFS */
#define SRV_KILL  	111	/* to PM: special kill call for RS */

#define GCOV_FLUSH	112	/* flush gcov data from server to gcov files */

#define PM_GETSID	113	/* PM getsid() */
#define CLOCK_GETRES	114	/* clock_getres() */
#define CLOCK_GETTIME	115	/* clock_gettime() */
#define CLOCK_SETTIME	116	/* clock_settime() */

#define VFS_VMCALL	117

#define MAPDRIVER      122     /* to VFS, map a device */
#define GETRUSAGE      123	/* to PM, VFS */

#define VFS_CHECKPERMS	124	/* to VFS */
