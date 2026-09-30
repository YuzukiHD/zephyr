/*
 * Copyright (c) 2026 Yuzuki Tsuru
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Singly linked first-in first-out queue. Any struct whose first member is a
 * `next` pointer can be queued; the decoders use it for their own nodes.
 */

#include <stddef.h>

struct ve_fifo_node {
	void *next;
	void *param1;
	void *param2;
};

void FIFOEnqueue(struct ve_fifo_node **head, struct ve_fifo_node *node)
{
	struct ve_fifo_node **tail = head;

	while (*tail != NULL) {
		tail = (struct ve_fifo_node **)&(*tail)->next;
	}
	node->next = NULL;
	*tail = node;
}

struct ve_fifo_node *FIFODequeue(struct ve_fifo_node **head)
{
	struct ve_fifo_node *node = *head;

	if (node != NULL) {
		*head = node->next;
		node->next = NULL;
	}

	return node;
}

void FIFOEnqueueToHead(struct ve_fifo_node **head, struct ve_fifo_node *node)
{
	node->next = *head;
	*head = node;
}
