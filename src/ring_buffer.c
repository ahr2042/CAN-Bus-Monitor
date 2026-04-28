/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * ring_buffer.c — Lock-free SPSC ring buffer implementation
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Implementation details:
 *  - head  : written only by the producer; read by the consumer.
 *  - tail  : written only by the consumer; read by the producer.
 *  - Both indices use _Atomic(size_t) with explicit memory ordering:
 *      • Producer: release store on head after writing payload.
 *      • Consumer: acquire load on head before reading payload;
 *                  release store on tail after consuming.
 *  - The buffer wastes one slot to distinguish full from empty
 *    (classic two-index SPSC design).
 */

#include "ring_buffer.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal structure
 * ------------------------------------------------------------------------- */
struct ring_buffer {
    _Atomic(size_t) head;   /**< Next write position (producer)  */
    _Atomic(size_t) tail;   /**< Next read  position (consumer)  */
    size_t          mask;   /**< capacity - 1; capacity is 2^n   */
    size_t          item_size;
    uint8_t        *data;   /**< Heap-allocated payload storage  */
};

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/** Round @n up to the next power of two.  Returns 1 if n == 0. */
static size_t next_pow2(size_t n)
{
    if (n == 0u) {
        return 1u;
    }
    --n;
    n |= n >> 1u;
    n |= n >> 2u;
    n |= n >> 4u;
    n |= n >> 8u;
    n |= n >> 16u;
    /* Handle 64-bit size_t */
#if SIZE_MAX > 0xFFFFFFFFu
    n |= n >> 32u;
#endif
    return n + 1u;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

ring_buffer_t *rb_create(size_t capacity, size_t item_size)
{
    if (item_size == 0u) {
        errno = EINVAL;
        return NULL;
    }

    ring_buffer_t *rb = calloc(1, sizeof(*rb));
    if (!rb) {
        return NULL;  /* errno already set by calloc */
    }

    /* Enforce power-of-two capacity; add one for the sentinel slot */
    size_t real_cap = next_pow2(capacity + 1u);
    rb->data = malloc(real_cap * item_size);
    if (!rb->data) {
        free(rb);
        return NULL;
    }

    rb->mask      = real_cap - 1u;
    rb->item_size = item_size;
    atomic_store_explicit(&rb->head, 0u, memory_order_relaxed);
    atomic_store_explicit(&rb->tail, 0u, memory_order_relaxed);

    return rb;
}

bool rb_push(ring_buffer_t *rb, const void *item)
{
    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t next = (head + 1u) & rb->mask;

    /* Check for full: if next == tail the buffer is full */
    if (next == atomic_load_explicit(&rb->tail, memory_order_acquire)) {
        return false;
    }

    /* Write payload then publish by advancing head */
    memcpy(rb->data + head * rb->item_size, item, rb->item_size);
    atomic_store_explicit(&rb->head, next, memory_order_release);
    return true;
}

bool rb_pop(ring_buffer_t *rb, void *out)
{
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);

    /* Check for empty: head == tail */
    if (tail == atomic_load_explicit(&rb->head, memory_order_acquire)) {
        return false;
    }

    /* Read payload then advance tail */
    memcpy(out, rb->data + tail * rb->item_size, rb->item_size);
    atomic_store_explicit(&rb->tail,
                          (tail + 1u) & rb->mask,
                          memory_order_release);
    return true;
}

size_t rb_size(const ring_buffer_t *rb)
{
    size_t head = atomic_load_explicit(&rb->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&rb->tail, memory_order_relaxed);
    return (head - tail) & rb->mask;
}

size_t rb_capacity(const ring_buffer_t *rb)
{
    /* Real allocated slots minus the sentinel */
    return rb->mask;
}

void rb_destroy(ring_buffer_t *rb)
{
    if (!rb) {
        return;
    }
    free(rb->data);
    free(rb);
}
