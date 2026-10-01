/*
 * driver_interface.h - ABI shared between the kernel-space smart meter driver
 *                      and the user-space analytics application.
 *
 * This header is compiled BOTH by the kernel module build (-D__KERNEL__) and
 * by the user-space build.  Every structure below is fixed-width and
 * naturally aligned so the layout is identical on both sides.
 *
 * Data path used by the application
 * ---------------------------------
 *   read(fd, &sm_sample, sizeof(sm_sample))  -> cumulative pulse count
 *   write(fd, &pulses, sizeof(pulses))      -> manual pulse injection
 *   poll(fd, ...)                           -> wait for the next pulse burst
 *   ioctl(fd, SM_IOC_*)                     -> stats / configuration
 *
 * NOTE: _IOR()/_IOW() come from <asm/ioctl.h>.  In kernel space that header is
 * pulled in by <linux/fs.h>, in user space by <sys/ioctl.h>.  Always include
 * one of those BEFORE this header.
 */
#ifndef SMART_METER_DRIVER_INTERFACE_H
#define SMART_METER_DRIVER_INTERFACE_H

#ifdef __KERNEL__
#include <linux/types.h>

typedef __u64 sm_u64;
typedef __u32 sm_u32;
typedef __s64 sm_s64;
#else
#include <stdint.h>

typedef uint64_t sm_u64;
typedef uint32_t sm_u32;
typedef int64_t sm_s64;
#endif /* __KERNEL__ */

#ifdef __cplusplus
extern "C" {
#endif

#define SM_DRIVER_VERSION "1.0.0"

/* Character device exposed by misc_register() */
#define SM_DEVICE_NAME "smart_meter"
#define SM_DEVICE_PATH "/dev/" SM_DEVICE_NAME

/* Optional kernel interfaces exposing the same counters */
#define SM_PROC_NAME "smart_meter"
#define SM_PROC_PATH "/proc/" SM_PROC_NAME
#define SM_SYSFS_DIR "/sys/kernel/" SM_DEVICE_NAME

/* Selects which pulse generator feeds the kernel counter. */
#define SM_SOURCE_NONE 0u /* module loaded, no source attached yet */
#define SM_SOURCE_SIM 1u  /* hrtimer based pulse generator (virtual meter) */
#define SM_SOURCE_GPIO 2u /* real GPIO edge interrupt from an external meter */

/*
 * One cumulative counter snapshot, the payload of read().
 * Written with a single copy_to_user() so it is atomic with respect to
 * other readers for a single-meter device.
 */
struct sm_sample {
    sm_u64 sequence;     /* incremented by the driver on every pulse        */
    sm_u64 timestamp_ns; /* CLOCK_MONOTONIC at the time of the snapshot     */
    sm_u64 pulse_count;  /* cumulative pulses since load / SM_IOC_RESET       */
};

/* Payload of SM_IOC_GET_STATS - diagnostic view of the driver's internals. */
struct sm_stats {
    sm_u64 pulse_count;      /* cumulative pulses                        */
    sm_u64 pulses_delta;    /* pulses accumulated since the last read()  */
    sm_u64 last_pulse_ns;    /* CLOCK_MONOTONIC of the last pulse edge   */
    sm_u64 driver_uptime_ns; /* CLOCK_MONOTONIC - module load time       */
    sm_u64 interrupt_count;  /* interrupt handler invocations             */
    sm_u64 simulated_count;  /* pulses produced by the hrtimer generator */
    sm_u64 injected_count;   /* pulses injected through write()           */
    sm_u32 source;           /* SM_SOURCE_*                              */
    sm_u32 enabled;          /* 1 = counter running, 0 = paused          */
};

/* Payload of SM_IOC_SET_CONFIG / SM_IOC_GET_CONFIG. */
struct sm_config {
    sm_u32 pulses_per_hour; /* simulated source rate, 1..1000000          */
    sm_u32 source;          /* SM_SOURCE_SIM or SM_SOURCE_GPIO            */
    sm_u32 enabled;         /* 1 = start / keep the counter running      */
    sm_u32 reserved;        /* padding, must be 0                         */
};

#define SM_SAMPLE_SIZE ((unsigned int)sizeof(struct sm_sample))
#define SM_STATS_SIZE ((unsigned int)sizeof(struct sm_stats))
#define SM_CONFIG_SIZE ((unsigned int)sizeof(struct sm_config))

#define SM_IOC_MAGIC 0x53 /* 'S' */

/* Full driver status: counter, uptime, per-source tallies. */
#define SM_IOC_GET_STATS _IOR(SM_IOC_MAGIC, 0x01, struct sm_stats)
/* Change simulated pulse rate and/or pause the counter. */
#define SM_IOC_SET_CONFIG _IOW(SM_IOC_MAGIC, 0x02, struct sm_config)
/* Read back the effective configuration. */
#define SM_IOC_GET_CONFIG _IOR(SM_IOC_MAGIC, 0x03, struct sm_config)
/* Zero the cumulative counter and all tallies. */
#define SM_IOC_RESET _IO(SM_IOC_MAGIC, 0x04)
/* Adjust the totaliser constant (pulses -> Wh) without rebuilding. */
#define SM_IOC_SET_CONSTANT _IOW(SM_IOC_MAGIC, 0x05, sm_u64)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SMART_METER_DRIVER_INTERFACE_H */
