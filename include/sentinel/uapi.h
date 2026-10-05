/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Sentinel user/kernel ABI.
 *
 * This header is included by BOTH the kernel module (driver/sentinel_drv.c)
 * and user space (daemon, CLI, tools), so it may only use <linux/...> types.
 * All structures use fixed-width types and have no implicit padding, which
 * keeps the ABI identical for 32-bit and 64-bit callers.
 */
#ifndef SENTINEL_UAPI_H
#define SENTINEL_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENTINEL_DEVICE_NAME "sentinel"

/* One sensor reading. Exactly what read(2) returns, N records at a time. */
struct sentinel_sample {
	__u64 ts_ns;    /* CLOCK_MONOTONIC timestamp, nanoseconds */
	__s32 temp_mc;  /* temperature, milli-degrees Celsius     */
	__s32 hum_mpct; /* relative humidity, milli-percent       */
	__u32 seq;      /* per-session sequence number            */
	__u32 flags;    /* SENTINEL_FLAG_*                        */
};

/* Set on a sample that directly follows one or more dropped samples. */
#define SENTINEL_FLAG_OVERRUN 0x1u

struct sentinel_dev_stats {
	__u64 produced;   /* samples generated this session        */
	__u64 dropped;    /* samples overwritten before being read */
	__u32 period_ms;  /* current sampling period               */
	__u32 fifo_depth; /* capacity in samples                   */
	__u32 fifo_used;  /* samples currently queued              */
	__u32 reserved;
};

#define SENTINEL_PERIOD_MIN_MS 10u
#define SENTINEL_PERIOD_MAX_MS 10000u

/* NOTE: demo magic number; register a real one (Documentation/userspace-api/ioctl)
 * before shipping a driver outside of a lab. */
#define SENTINEL_IOC_MAGIC 0xE5

#define SENTINEL_IOC_SET_PERIOD _IOW(SENTINEL_IOC_MAGIC, 1, __u32)
#define SENTINEL_IOC_GET_STATS  _IOR(SENTINEL_IOC_MAGIC, 2, struct sentinel_dev_stats)
#define SENTINEL_IOC_RESET      _IO(SENTINEL_IOC_MAGIC, 3)

#ifdef __cplusplus
}
#endif

#endif /* SENTINEL_UAPI_H */
