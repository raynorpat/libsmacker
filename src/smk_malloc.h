/**
	libsmacker - A C library for decoding .smk Smacker Video files
	Copyright (C) 2012-2017 Greg Kennedy

	See smacker.h for more information.

	smk_malloc.h
		"Safe" implementations of malloc and free.
*/

#ifndef SMK_MALLOC_H
#define SMK_MALLOC_H

/* assert */
#include <assert.h>
/* calloc */
#include <stdlib.h>
/* fprintf */
#include <stdio.h>

/* Error messages from calloc */
#include <errno.h>
#include <string.h>

/**
	Safe free: prevents double-free by setting the pointer to NULL.
		Freeing a NULL pointer is allowed, so partially built objects can be
		torn down on an error path.
*/
#define smk_free(p) \
{ \
	free(p); \
	p = NULL; \
}

/**
	Safe malloc: allocates a zeroed block of at least one byte.
		Sizes come from file headers, so a failed allocation is an ordinary
		error: report it and jump to the caller's `error` label.
*/
#define smk_malloc(p, x) \
{ \
	assert (p == NULL); \
	p = calloc(1, (x) != 0 ? (x) : 1); \
	if (!p) \
	{ \
		fprintf(stderr, "libsmacker::smk_malloc(" #p ", %lu) - ERROR: calloc() returned NULL (file: %s, line: %lu)\n\tReason: [%d] %s\n", \
			(unsigned long) (x), __FILE__, (unsigned long)__LINE__, errno, strerror(errno)); \
		goto error; \
	} \
}

#endif
