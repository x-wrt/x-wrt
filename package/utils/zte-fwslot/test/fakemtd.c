// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test shim for zte-fwslot: LD_PRELOAD this to run the unmodified tool against
 * plain files that stand in for the two firmware slot partitions.
 *
 *   FAKE_ROOT         directory holding dev/mtdN, sys/class/mtd/mtdN/offset and
 *                     sys/firmware/zx279133/slot_offset
 *   FAKE_BAD          "N:0xofs,N:0xofs" blocks the bad block table lists
 *   FAKE_WRITE_FAIL   "N:0xofs" blocks that refuse to be written
 *   FAKE_CORRUPT      "N:0xofs" blocks that silently corrupt a successful write
 *
 * MEMSETBADBLOCK is remembered in $FAKE_ROOT/badlist so that later runs of the
 * tool see the block as bad, like a real bad block table.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mtd/mtd-user.h>

#define ERASESIZE 0x20000
#define MAXFD 1024

static int fd_dev[MAXFD];	/* mtd index + 1 of a tracked fd, else 0 */

static const char *root(void)
{
	const char *r = getenv("FAKE_ROOT");

	if (!r) {
		fprintf(stderr, "fakemtd: FAKE_ROOT is not set\n");
		exit(99);
	}
	return r;
}

static const char *map(const char *path, char *buf, size_t n)
{
	static const char *const prefix[] = { "/dev/mtd", "/sys/class/mtd",
					      "/sys/firmware/zx279133" };

	for (unsigned i = 0; i < sizeof(prefix) / sizeof(prefix[0]); i++) {
		if (!strncmp(path, prefix[i], strlen(prefix[i]))) {
			snprintf(buf, n, "%s%s", root(), path);
			return buf;
		}
	}
	return path;
}

static int in_list(const char *env, int dev, unsigned long long ofs)
{
	const char *p = getenv(env);

	while (p && *p) {
		int d;
		unsigned long long o;
		int used = 0;

		if (sscanf(p, "%d:%llx%n", &d, &o, &used) >= 2 && d == dev &&
		    ofs / ERASESIZE == o / ERASESIZE)
			return 1;
		p = strchr(p, ',');
		if (p)
			p++;
	}
	return 0;
}

int system(const char *command)
{
	int (*real_system)(const char *) = dlsym(RTLD_NEXT, "system");

	if (command && !strcmp(command, "/usr/libexec/zte-stage-overlay"))
		return real_system("exec \"$FAKE_ROOT/usr/libexec/zte-stage-overlay\"");
	return real_system(command);
}

static int in_badlist(int dev, unsigned long long ofs)
{
	char path[512], line[64];
	FILE *f;
	int hit = 0;

	snprintf(path, sizeof(path), "%s/badlist", root());
	f = fopen(path, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		int d;
		unsigned long long o;

		if (sscanf(line, "%d %llx", &d, &o) == 2 && d == dev &&
		    ofs / ERASESIZE == o / ERASESIZE)
			hit = 1;
	}
	fclose(f);
	return hit;
}

static int track(int fd, const char *orig)
{
	int idx;

	if (fd >= 0 && fd < MAXFD && sscanf(orig, "/dev/mtd%d", &idx) == 1)
		fd_dev[fd] = idx + 1;
	return fd;
}

int open(const char *path, int flags, ...)
{
	static int (*real)(const char *, int, ...);
	char buf[512];
	mode_t mode = 0;

	if (!real)
		real = dlsym(RTLD_NEXT, "open");
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);
	}
	return track(real(map(path, buf, sizeof(buf)), flags, mode), path);
}
int open64(const char *path, int flags, ...) __attribute__((alias("open")));

#ifdef __GLIBC__
/* glibc uses these entry points for fortified opens with no mode argument. */
int __open_2(const char *path, int flags)
{
	return open(path, flags);
}

int __open64_2(const char *path, int flags)
{
	return open(path, flags);
}
#endif

FILE *fopen(const char *path, const char *mode)
{
	static FILE *(*real)(const char *, const char *);
	char buf[512];

	if (!real)
		real = dlsym(RTLD_NEXT, "fopen");
	return real(map(path, buf, sizeof(buf)), mode);
}
FILE *fopen64(const char *path, const char *mode) __attribute__((alias("fopen")));

DIR *opendir(const char *path)
{
	static DIR *(*real)(const char *);
	char buf[512];

	if (!real)
		real = dlsym(RTLD_NEXT, "opendir");
	return real(map(path, buf, sizeof(buf)));
}

#ifdef __GLIBC__
typedef unsigned long ioctl_request_t;
#else
typedef int ioctl_request_t;
#endif

int ioctl(int fd, ioctl_request_t req, ...)
{
	static int (*real)(int, ioctl_request_t, ...);
	va_list ap;
	void *arg;
	int dev = (fd >= 0 && fd < MAXFD) ? fd_dev[fd] - 1 : -1;

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (dev < 0) {
		if (!real)
			real = dlsym(RTLD_NEXT, "ioctl");
		return real(fd, req, arg);
	}

	switch ((unsigned)req) {
	case (unsigned)MEMGETINFO: {
		struct mtd_info_user *i = arg;
		struct stat st;

		fstat(fd, &st);
		memset(i, 0, sizeof(*i));
		i->type = MTD_NANDFLASH;
		i->size = st.st_size;
		i->erasesize = ERASESIZE;
		i->writesize = 2048;
		i->oobsize = 64;
		return 0;
	}
	case (unsigned)MEMGETBADBLOCK: {
		unsigned long long ofs = *(long long *)arg;

		return in_list("FAKE_BAD", dev, ofs) || in_badlist(dev, ofs);
	}
	case (unsigned)MEMSETBADBLOCK: {
		char path[512];
		FILE *f;

		snprintf(path, sizeof(path), "%s/badlist", root());
		f = fopen(path, "a");
		if (!f)
			return -1;
		fprintf(f, "%d %llx\n", dev, *(unsigned long long *)arg);
		fclose(f);
		return 0;
	}
	case (unsigned)MEMUNLOCK:
		return 0;
	case (unsigned)MEMERASE: {
		struct erase_info_user *e = arg;
		static unsigned char ff[ERASESIZE];

		if (!ff[0])
			memset(ff, 0xff, sizeof(ff));
		if (e->start % ERASESIZE || e->length % ERASESIZE) {
			errno = EINVAL;
			return -1;
		}
		for (unsigned long long o = e->start; o < e->start + e->length; o += ERASESIZE)
			if (pwrite(fd, ff, ERASESIZE, o) != ERASESIZE)
				return -1;
		return 0;
	}
	}
	errno = ENOTTY;
	return -1;
}

ssize_t write(int fd, const void *buf, size_t n)
{
	static ssize_t (*real)(int, const void *, size_t);
	int dev = (fd >= 0 && fd < MAXFD) ? fd_dev[fd] - 1 : -1;
	off_t ofs = dev >= 0 ? lseek(fd, 0, SEEK_CUR) : -1;
	ssize_t written;

	if (!real)
		real = dlsym(RTLD_NEXT, "write");
	if (dev >= 0 && in_list("FAKE_WRITE_FAIL", dev, ofs)) {
		errno = EIO;
		return -1;
	}
	if (dev >= 0 && in_list("FAKE_WRITE_RO", dev, ofs)) {
		errno = EROFS;
		return -1;
	}
	written = real(fd, buf, n);
	if (written > 0 && dev >= 0 && in_list("FAKE_CORRUPT", dev, ofs)) {
		unsigned char flipped = *(const unsigned char *)buf ^ 1;

		if (pwrite(fd, &flipped, 1, ofs) != 1)
			return -1;
	}
	return written;
}
