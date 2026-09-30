/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Mutexes for the archive. Its pthread_mutex_t is a 40 byte block that is all
 * zero when idle; the first word holds the address of a k_mutex that is
 * created on the first lock or init.
 */

#include <stdint.h>
#include <zephyr/kernel.h>

static struct k_spinlock create_lock;

static struct k_mutex *mutex_of(uint32_t *m)
{
	struct k_mutex *mu = (struct k_mutex *)(uintptr_t)*m;

	if (mu == NULL) {
		k_spinlock_key_t key = k_spin_lock(&create_lock);

		mu = (struct k_mutex *)(uintptr_t)*m;
		if (mu == NULL) {
			mu = k_malloc(sizeof(*mu));
			if (mu != NULL) {
				k_mutex_init(mu);
				*m = (uint32_t)(uintptr_t)mu;
			}
		}
		k_spin_unlock(&create_lock, key);
	}

	return mu;
}

int pthread_mutex_init(uint32_t *m, const void *attr)
{
	(void)attr;
	*m = 0;

	return mutex_of(m) != NULL ? 0 : -1;
}

int pthread_mutex_lock(uint32_t *m)
{
	struct k_mutex *mu = mutex_of(m);

	return (mu != NULL && k_mutex_lock(mu, K_FOREVER) == 0) ? 0 : -1;
}

int pthread_mutex_unlock(uint32_t *m)
{
	struct k_mutex *mu = mutex_of(m);

	return (mu != NULL && k_mutex_unlock(mu) == 0) ? 0 : -1;
}

int pthread_mutex_destroy(uint32_t *m)
{
	k_free((void *)(uintptr_t)*m);
	*m = 0;

	return 0;
}
