/*
 * nandwrite.c - ported to busybox from mtd-utils
 *
 * Author: Baruch Siach <baruch@tkos.co.il>, Orex Computed Radiography
 *
 * Licensed under GPLv2, see file LICENSE in this source tree.
 *
 * TODO: add support for large (>4GB) MTD devices
 */

//applet:IF_NANDWRITE(APPLET(nandwrite, _BB_DIR_USR_SBIN, _BB_SUID_DROP))

//kbuild:lib-$(CONFIG_NANDWRITE) += nandwrite.o

//config:config NANDWRITE
//config:	bool "nandwrite"
//config:	default n
//config:	depends on PLATFORM_LINUX
//config:	help
//config:	  Write to the specified MTD device, with bad blocks awareness

#include "libbb.h"
#include <mtd/mtd-user.h>

//usage:#define nandwrite_trivial_usage
//usage:	"[-p] [-s ADDR] MTD_DEVICE [FILE]"
//usage:#define nandwrite_full_usage "\n\n"
//usage:	"Write to the specified MTD device\n"
//usage:     "\nOptions:"
//usage:     "\n	-p	Pad to page size"
//usage:     "\n	-s ADDR	Start address"

static unsigned next_good_eraseblock(int fd, struct mtd_info_user *meminfo,
		unsigned block_offset)
{
	while (1) {
		loff_t offs;
		if (block_offset >= meminfo->size)
			bb_error_msg_and_die("not enough space in MTD device");
		offs = block_offset;
		if (xioctl(fd, MEMGETBADBLOCK, &offs) == 0)
			return block_offset;
		/* ioctl returned 1 => "bad block" */
		printf("Skipping bad block at 0x%08x\n", block_offset);
		block_offset += meminfo->erasesize;
	}
}

int nandwrite_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int nandwrite_main(int argc UNUSED_PARAM, char **argv)
{
	unsigned opts;
	int fd;
	ssize_t cnt;
	unsigned mtdoffset, meminfo_writesize, blockstart;
	struct mtd_info_user meminfo;
	unsigned char *filebuf;
	const char *opt_s = "0";
	enum {
		OPT_p = (1 << 0),
		OPT_s = (1 << 1),
	};

	opt_complementary = "-1:?2";
	opts = getopt32(argv, "ps:", &opt_s);
	argv += optind;

	if (argv[1])
		xmove_fd(xopen_stdin(argv[1]), STDIN_FILENO);

	fd = xopen(argv[0], O_RDWR);
	xioctl(fd, MEMGETINFO, &meminfo);

	mtdoffset = bb_strtou(opt_s, NULL, 0);
	if (errno)
		bb_error_msg_and_die("invalid number '%s'", opt_s);

	/* Pull it into a CPU register (hopefully) - smaller code that way */
	meminfo_writesize = meminfo.writesize;

	if (mtdoffset & (meminfo_writesize - 1))
		bb_error_msg_and_die("start address is not page aligned");

	filebuf = xmalloc(meminfo_writesize);

	blockstart = mtdoffset & ~(meminfo.erasesize - 1);
	if (blockstart != mtdoffset) {
		unsigned tmp;
		/* mtdoffset is in the middle of an erase block, verify that
		 * this block is OK. Advance mtdoffset only if this block is
		 * bad.
		 */
		tmp = next_good_eraseblock(fd, &meminfo, blockstart);
		if (tmp != blockstart) /* bad block(s), advance mtdoffset */
			mtdoffset = tmp;
	}

	cnt = -1;
	while (mtdoffset < meminfo.size) {
		blockstart = mtdoffset & ~(meminfo.erasesize - 1);
		if (blockstart == mtdoffset) {
			/* starting a new eraseblock */
			mtdoffset = next_good_eraseblock(fd, &meminfo, blockstart);
			printf("Writing at 0x%08x\n", mtdoffset);
		}
		/* get some more data from input */
		cnt = full_read(STDIN_FILENO, filebuf, meminfo_writesize);
		if (cnt == 0) {
			/* even with -p, we do not pad past the end of input
			 * (-p only zero-pads last incomplete page)
			 */
			break;
		}
		if (cnt < meminfo_writesize) {
			if (!(opts & OPT_p))
				bb_error_msg_and_die("input size is not rounded up to page size, "
						"use -p to zero pad");
			/* zero pad to end of write block */
			memset(filebuf + cnt, 0, meminfo_writesize - cnt);
		}
		xlseek(fd, mtdoffset, SEEK_SET);
		xwrite(fd, filebuf, meminfo_writesize);
		mtdoffset += meminfo_writesize;
		if (cnt < meminfo_writesize)
			break;
	}

	if (cnt != 0) {
		/* We filled entire MTD, but did we reach EOF on input? */
		if (full_read(STDIN_FILENO, filebuf, meminfo_writesize) != 0) {
			/* no */
			bb_error_msg_and_die("not enough space in MTD device");
		}
	}

	if (ENABLE_FEATURE_CLEAN_UP) {
		free(filebuf);
		close(fd);
	}

	return EXIT_SUCCESS;
}
