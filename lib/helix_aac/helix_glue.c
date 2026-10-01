/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

void *helix_malloc(int size)
{
	void *p = malloc(size);

	if (p != NULL) {
		memset(p, 0, size);
	}

	return p;
}

void helix_free(void *ptr)
{
	free(ptr);
}
