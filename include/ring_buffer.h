/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ring_buffer.h — Lock-free single-producer / single-consumer ring buffer
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Classic SPSC ring buffer using C11 atomics for cache-line-friendly
 * communication between the CAN receive thread (producer) and the
 * logger flush thread (consumer).  No mutexes → zero blocking on the
 * hot receive path.
 *
 * Capacity must be a power of two so that index wrapping can be done
 * with a bitwise AND instead of modulo.
 *
 * Usage:
 *   ring_buffer_t *rb = rb_create(256, sizeof(my_item_t));
 *   rb_push(rb, &item);   // producer side
 *   rb_pop(rb, &item);    // consumer side
 *   rb_destroy(rb);
 */

#ifndef CANBUS_MONITOR_RING_BUFFER_H
#define CANBUS_MONITOR_RING_BUFFER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque ring-buffer handle */
typedef struct ring_buffer ring_buffer_t;

/**
 * rb_create() — Allocate a ring buffer.
 *
 * @param capacity  Number of slots.  Will be rounded up to the next power
 *                  of two if not already a power of two.
 * @param item_size Size (bytes) of each item stored in the buffer.
 * @return  Handle on success, NULL on allocation failure (errno set).
 */
ring_buffer_t *rb_create(size_t capacity, size_t item_size);

/**
 * rb_push() — Copy @item into the next free slot (producer side).
 *
 * @return  true if successful, false if the buffer is full (item dropped).
 */
bool rb_push(ring_buffer_t *rb, const void *item);

/**
 * rb_pop() — Copy the oldest item into @out and advance the read index.
 *
 * @return  true if an item was available, false if the buffer is empty.
 */
bool rb_pop(ring_buffer_t *rb, void *out);

/**
 * rb_size() — Approximate number of items currently in the buffer.
 *
 * Because producer and consumer run concurrently the value may be stale
 * by the time the caller uses it.  Suitable for monitoring only.
 */
size_t rb_size(const ring_buffer_t *rb);

/**
 * rb_capacity() — Total number of slots in the buffer.
 */
size_t rb_capacity(const ring_buffer_t *rb);

/**
 * rb_destroy() — Release all memory.  Passing NULL is a no-op.
 */
void rb_destroy(ring_buffer_t *rb);

#ifdef __cplusplus
}
#endif

#endif /* CANBUS_MONITOR_RING_BUFFER_H */
