/*
 * ipc_channel.h - POSIX IPC layer (shared memory ring + message queue).
 *
 * Two distinct mechanisms are demonstrated:
 *
 *   1. Shared memory ring buffer (shm_open + ftruncate + mmap): the fast
 *      path used to stream pulse snapshots between the acquisition
 *      process (the dashboard) and any number of consumer processes.
 *      Lock-free: producer and consumer advance independent indices using
 *      GCC/Clang atomic builtins, which is safe across processes because
 *      the shared page is naturally aligned.
 *
 *   2. POSIX message queue (mq_open/mq_send/mq_receive): the slow,
 *      control path used for commands such as "reset counter" or
 *      "change pulse rate", where delivery guarantees matter more than
 *      throughput.
 *
 * Wire structures are kept C-compatible on purpose so a C client can use
 * the same layout.
 */
#ifndef SMART_METER_IPC_CHANNEL_H
#define SMART_METER_IPC_CHANNEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SM_IPC_MAGIC 0x474D5353u /* "SMSG" */
#define SM_IPC_VERSION 1u

#define SM_IPC_DEFAULT_SHM "/smart_meter_pulses"
#define SM_IPC_DEFAULT_MQ "/smart_meter_cmd"

/* One sample as stored in shared memory. */
struct sm_ipc_sample {
    uint64_t sequence;
    uint64_t timestamp_ns;   /* CLOCK_MONOTONIC at production time */
    uint64_t epoch_seconds;  /* CLOCK_REALTIME at production time   */
    uint64_t pulse_count;
};

/* Header placed at offset 0 of the shared memory object; slots follow. */
struct sm_ipc_header {
    uint32_t magic;
    uint32_t version;
    uint32_t _pad0;
    uint32_t _pad1;
    uint64_t capacity;       /* number of slots                      */
    uint64_t slot_size;      /* sizeof(struct sm_ipc_sample)         */
    uint64_t write_index;    /* total samples published              */
    uint64_t read_index;     /* total samples consumed               */
    uint64_t dropped;        /* samples overwritten before being read */
    uint64_t last_update_ns; /* CLOCK_REALTIME of last publication   */
};

/* --- shared memory ring -------------------------------------------- */

/* Producer side: create (or truncate) the object and initialise it. */
int sm_ipc_shm_create(const char* name, uint32_t capacity, char* err, size_t err_len);
/* Consumer side: map an existing object. */
int sm_ipc_shm_attach(const char* name, char* err, size_t err_len);
void sm_ipc_shm_detach(void);

/* Producer. Returns 1 on success, 0 when the ring is full (counted as a
 * drop, oldest slot overwritten), -1 on error. */
int sm_ipc_shm_publish(const struct sm_ipc_sample* sample);
/* Consumer. Returns 1 when a sample was copied out, 0 when empty. */
int sm_ipc_shm_consume(struct sm_ipc_sample* out);

/* Introspection for the dashboard. */
uint64_t sm_ipc_shm_available(void);
uint64_t sm_ipc_shm_produced(void);
uint64_t sm_ipc_shm_dropped(void);
int sm_ipc_shm_is_attached(void);
const char* sm_ipc_shm_name(void);

/* --- POSIX message queue ------------------------------------------- */

int sm_ipc_mq_create(const char* name, long max_messages, long message_bytes,
                     char* err, size_t err_len);
int sm_ipc_mq_open(const char* name, char* err, size_t err_len);
void sm_ipc_mq_close(void);
int sm_ipc_mq_send(const char* payload);
int sm_ipc_mq_receive(char* buffer, size_t buffer_len, unsigned timeout_ms);
int sm_ipc_mq_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* SMART_METER_IPC_CHANNEL_H */
