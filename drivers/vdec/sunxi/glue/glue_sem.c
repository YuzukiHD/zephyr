/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Counting semaphores handed to the archive as opaque pointers */

#include <stdlib.h>
#include <zephyr/kernel.h>

struct glue_sem {
	struct k_sem sem;
};

void *hal_sem_create(unsigned int cnt)
{
	struct glue_sem *s = k_malloc(sizeof(*s));

	if (s != NULL) {
		k_sem_init(&s->sem, cnt, K_SEM_MAX_LIMIT);
	}

	return s;
}

int hal_sem_delete(void *sem)
{
	k_free(sem);
	return 0;
}

int hal_sem_post(void *sem)
{
	k_sem_give(&((struct glue_sem *)sem)->sem);
	return 0;
}

/* The archive counts in milliseconds; the largest value means forever. */
int hal_sem_timedwait(void *sem, unsigned long ms)
{
	k_timeout_t to = (ms >= 0xffffffffUL) ? K_FOREVER : K_MSEC(ms);

	return k_sem_take(&((struct glue_sem *)sem)->sem, to) == 0 ? 0 : -1;
}

int hal_sem_getvalue(void *sem, int *val)
{
	*val = (int)k_sem_count_get(&((struct glue_sem *)sem)->sem);
	return 0;
}
