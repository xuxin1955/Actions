/* SPDX-License-Identifier: GPL-2.0-only */
#include "cpnv.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "--serve")) {
		fprintf(stderr, "Usage: udx-cpnvd --serve\n");
		return 64;
	}
	umask(077);
	if (mkdir("/run/udx-cpnv-warm", 0700) && errno != EEXIST) {
		perror("cpnv runtime directory");
		return 1;
	}
	struct stat directory;
	if (lstat("/run/udx-cpnv-warm", &directory) || !S_ISDIR(directory.st_mode) ||
	    directory.st_uid || (directory.st_mode & 0077)) {
		fprintf(stderr, "cpnv: runtime directory permissions invalid\n");
		return 1;
	}
	struct cpnv_policy policy;
	int e = cpnv_policy_load(&policy);
	if (e) {
		fprintf(stderr, "cpnv: configuration error=%d\n", e);
		return 1;
	}
	struct cpnv context;
	e = cpnv_service_run(&context, &policy);
	if (e)
		fprintf(stderr, "cpnv: stopped error=%d\n", e);
	return e ? 1 : 0;
}
