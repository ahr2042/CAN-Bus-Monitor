/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * can_socket.c — SocketCAN interface abstraction
 *
 * Copyright (C) 2026  Ahmad Rashed
 *
 * Opens an AF_CAN / SOCK_RAW socket, requests hardware timestamps via
 * SO_TIMESTAMPING, and provides a clean receive wrapper.
 *
 * References:
 *   - linux/can.h, linux/can/raw.h
 *   - linux/sockios.h (SIOCGIFINDEX)
 *   - kernel Documentation/networking/can.rst
 */

#include "can_socket.h"

#include <errno.h>
#include <net/if.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/* SocketCAN headers */
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/errqueue.h>
#include <linux/net_tstamp.h>

/* -------------------------------------------------------------------------
 * Internal structure
 * ------------------------------------------------------------------------- */
struct can_socket {
    int          fd;            /**< Raw socket file descriptor      */
    unsigned int timeout_ms;    /**< Configured receive timeout      */
};

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/**
 * set_receive_timeout() — Configure SO_RCVTIMEO on the socket.
 *
 * A timeout of 0 clears the timeout (blocks indefinitely).
 */
static int set_receive_timeout(int fd, unsigned int ms)
{
    struct timeval tv = {
        .tv_sec  = (time_t)(ms / 1000u),
        .tv_usec = (suseconds_t)((ms % 1000u) * 1000u),
    };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        return -errno;
    }
    return 0;
}

/**
 * enable_hardware_timestamps() — Request kernel/hardware timestamps.
 *
 * SOF_TIMESTAMPING_RX_HARDWARE  : use NIC hardware stamp if available.
 * SOF_TIMESTAMPING_RX_SOFTWARE  : fall back to software stamp.
 * SOF_TIMESTAMPING_SOFTWARE     : report software stamp.
 * SOF_TIMESTAMPING_RAW_HARDWARE : report raw hardware stamp.
 *
 * Timestamps are retrieved via recvmsg() ancillary data (CMSG_SPACE).
 * Failure here is non-fatal: we fall back to gettimeofday().
 */
static void enable_hardware_timestamps(int fd)
{
    int flags = SOF_TIMESTAMPING_RX_HARDWARE |
                SOF_TIMESTAMPING_RX_SOFTWARE  |
                SOF_TIMESTAMPING_SOFTWARE     |
                SOF_TIMESTAMPING_RAW_HARDWARE;
    (void)setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags));
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

can_socket_t *can_socket_open(const char *iface, unsigned int timeout_ms)
{
    if (!iface || iface[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }

    /* Allocate handle */
    can_socket_t *cs = calloc(1, sizeof(*cs));
    if (!cs) {
        return NULL;
    }

    /* Open a raw CAN socket */
    cs->fd = socket(AF_CAN, SOCK_RAW, CAN_RAW);
    if (cs->fd < 0) {
        free(cs);
        return NULL;
    }

    /* Look up interface index */
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1u);
    if (ioctl(cs->fd, SIOCGIFINDEX, &ifr) < 0) {
        int saved = errno;
        close(cs->fd);
        free(cs);
        errno = saved;
        return NULL;
    }

    /* Bind to the interface */
    struct sockaddr_can addr = {
        .can_family  = AF_CAN,
        .can_ifindex = ifr.ifr_ifindex,
    };
    if (bind(cs->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        int saved = errno;
        close(cs->fd);
        free(cs);
        errno = saved;
        return NULL;
    }

    /* Enable also error frames so the app can detect bus-off etc. */
    can_err_mask_t err_mask = CAN_ERR_MASK;
    (void)setsockopt(cs->fd, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
                     &err_mask, sizeof(err_mask));

    /* Request timestamps (best-effort) */
    enable_hardware_timestamps(cs->fd);

    /* Set receive timeout */
    cs->timeout_ms = timeout_ms;
    if (timeout_ms > 0u) {
        if (set_receive_timeout(cs->fd, timeout_ms) < 0) {
            /* Non-fatal: we proceed without timeout */
            cs->timeout_ms = 0u;
        }
    }

    return cs;
}

int can_socket_set_filters(can_socket_t *cs,
                           const can_filter_spec_t *filters,
                           size_t count)
{
    if (!cs || (!filters && count > 0u)) {
        return -EINVAL;
    }

    if (count == 0u) {
        /* Remove all filters → drop all frames */
        if (setsockopt(cs->fd, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0) < 0) {
            return -errno;
        }
        return 0;
    }

    /* Build the kernel filter array from our abstraction */
    struct can_filter *kfilters = malloc(count * sizeof(*kfilters));
    if (!kfilters) {
        return -errno;
    }

    for (size_t i = 0u; i < count; ++i) {
        kfilters[i].can_id   = filters[i].id;
        kfilters[i].can_mask = filters[i].mask;
    }

    int rc = setsockopt(cs->fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                        kfilters,
                        (socklen_t)(count * sizeof(*kfilters)));
    free(kfilters);
    if (rc < 0) {
        return -errno;
    }
    return 0;
}

int can_socket_recv(can_socket_t *cs, struct can_frame *frame, uint64_t *ts_ns)
{
    /* Use recvmsg to retrieve ancillary timestamp data */
    struct iovec  iov   = { .iov_base = frame, .iov_len = sizeof(*frame) };
    uint8_t       cmsg_buf[CMSG_SPACE(sizeof(struct scm_timestamping))];
    struct msghdr msg   = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1u,
        .msg_control    = cmsg_buf,
        .msg_controllen = sizeof(cmsg_buf),
    };

    ssize_t n = recvmsg(cs->fd, &msg, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;   /* Timeout */
        }
        return -errno;
    }
    if ((size_t)n < sizeof(*frame)) {
        return -EIO;    /* Short read — should not happen on SocketCAN */
    }

    /* Extract timestamp from ancillary data */
    *ts_ns = 0u;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
         cm != NULL;
         cm = CMSG_NXTHDR(&msg, cm)) {

        if (cm->cmsg_level != SOL_SOCKET) {
            continue;
        }
        if (cm->cmsg_type == SO_TIMESTAMPING) {
            /* scm_timestamping contains three struct timespec:
             *   [0] software timestamp
             *   [1] deprecated (zero)
             *   [2] hardware timestamp (raw)
             * Prefer hardware ([2]); fall back to software ([0]).
             */
            struct scm_timestamping *tss =
                (struct scm_timestamping *)(void *)CMSG_DATA(cm);
            struct timespec *ts = (tss->ts[2].tv_sec != 0)
                                ? &tss->ts[2]
                                : &tss->ts[0];
            *ts_ns = (uint64_t)ts->tv_sec * 1000000000ULL +
                     (uint64_t)ts->tv_nsec;
            break;
        }
    }

    /* Fallback: CLOCK_REALTIME if no ancillary timestamp */
    if (*ts_ns == 0u) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        *ts_ns = (uint64_t)now.tv_sec * 1000000000ULL +
                 (uint64_t)now.tv_nsec;
    }

    return 1;
}

int can_socket_get_fd(const can_socket_t *cs)
{
    return cs ? cs->fd : -1;
}

void can_socket_close(can_socket_t *cs)
{
    if (!cs) {
        return;
    }
    if (cs->fd >= 0) {
        close(cs->fd);
    }
    free(cs);
}
