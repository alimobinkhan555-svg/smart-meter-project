/*
 * ipc_channel.cpp - implementation of the shared memory ring and the POSIX
 * message queue used for inter-process communication.
 */
#include "ipc_channel.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {

struct Mapping {
    int fd{-1};
    struct sm_ipc_header* header{nullptr};
    unsigned char* slots{nullptr};
    size_t map_size{0};
    char name[128] = {0};
};

Mapping g_mapping;
mqd_t g_mqd = static_cast<mqd_t>(-1);

void setError(char* err, size_t err_len, const char* prefix) {
    if (err == nullptr || err_len == 0) return;
    snprintf(err, err_len, "%s: %s", prefix, strerror(errno));
}

struct sm_ipc_sample* slotAt(uint64_t index) {
    const uint64_t capacity = __atomic_load_n(&g_mapping.header->capacity, __ATOMIC_ACQUIRE);
    if (capacity == 0) return nullptr;
    const uint64_t offset = index % capacity;
    return reinterpret_cast<struct sm_ipc_sample*>(
        g_mapping.slots + offset * sizeof(struct sm_ipc_sample));
}

uint64_t realtimeNs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace

extern "C" {

/* ------------------------------------------------------------------ */
/* shared memory ring                                                  */
/* ------------------------------------------------------------------ */

int sm_ipc_shm_create(const char* name, uint32_t capacity, char* err, size_t err_len) {
    if (capacity == 0) capacity = 256;
    if (name == nullptr || name[0] == '\0') {
        if (err && err_len) snprintf(err, err_len, "shm name is empty");
        return -1;
    }

    sm_ipc_shm_detach();
    /* O_CREAT is used so the object always exists after this call; an
     * existing object is reused when it is big enough, otherwise it is
     * truncated and re-initialised (the producer is authoritative). */
    g_mapping.fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (g_mapping.fd < 0) {
        setError(err, err_len, "shm_open");
        return -1;
    }

    const size_t map_size = sizeof(struct sm_ipc_header) +
                            static_cast<size_t>(capacity) * sizeof(struct sm_ipc_sample);
    if (ftruncate(g_mapping.fd, static_cast<off_t>(map_size)) != 0) {
        setError(err, err_len, "ftruncate");
        ::close(g_mapping.fd);
        g_mapping.fd = -1;
        return -1;
    }

    void* base = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_mapping.fd, 0);
    if (base == MAP_FAILED) {
        setError(err, err_len, "mmap");
        ::close(g_mapping.fd);
        g_mapping.fd = -1;
        return -1;
    }

    g_mapping.header = static_cast<struct sm_ipc_header*>(base);
    g_mapping.slots = static_cast<unsigned char*>(base) + sizeof(struct sm_ipc_header);
    g_mapping.map_size = map_size;
    snprintf(g_mapping.name, sizeof(g_mapping.name), "%s", name);

    g_mapping.header->magic = SM_IPC_MAGIC;
    g_mapping.header->version = SM_IPC_VERSION;
    g_mapping.header->_pad0 = 0;
    g_mapping.header->_pad1 = 0;
    g_mapping.header->capacity = capacity;
    g_mapping.header->slot_size = sizeof(struct sm_ipc_sample);
    g_mapping.header->write_index = 0;
    g_mapping.header->read_index = 0;
    g_mapping.header->dropped = 0;
    g_mapping.header->last_update_ns = realtimeNs();
    return 0;
}

int sm_ipc_shm_attach(const char* name, char* err, size_t err_len) {
    if (name == nullptr || name[0] == '\0') {
        if (err && err_len) snprintf(err, err_len, "shm name is empty");
        return -1;
    }

    sm_ipc_shm_detach();
    g_mapping.fd = shm_open(name, O_RDWR, 0666);
    if (g_mapping.fd < 0) {
        setError(err, err_len, "shm_open");
        return -1;
    }

    struct stat info;
    if (fstat(g_mapping.fd, &info) != 0) {
        setError(err, err_len, "fstat");
        ::close(g_mapping.fd);
        g_mapping.fd = -1;
        return -1;
    }
    if (static_cast<size_t>(info.st_size) <= sizeof(struct sm_ipc_header)) {
        if (err && err_len) snprintf(err, err_len, "%s is not initialised", name);
        ::close(g_mapping.fd);
        g_mapping.fd = -1;
        return -1;
    }

    void* base = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ | PROT_WRITE,
                      MAP_SHARED, g_mapping.fd, 0);
    if (base == MAP_FAILED) {
        setError(err, err_len, "mmap");
        ::close(g_mapping.fd);
        g_mapping.fd = -1;
        return -1;
    }

    g_mapping.header = static_cast<struct sm_ipc_header*>(base);
    g_mapping.slots = static_cast<unsigned char*>(base) + sizeof(struct sm_ipc_header);
    g_mapping.map_size = static_cast<size_t>(info.st_size);
    snprintf(g_mapping.name, sizeof(g_mapping.name), "%s", name);

    if (g_mapping.header->magic != SM_IPC_MAGIC) {
        if (err && err_len) snprintf(err, err_len, "%s has a bad magic value", name);
        sm_ipc_shm_detach();
        return -1;
    }
    if (g_mapping.header->capacity == 0 ||
        g_mapping.header->capacity > (g_mapping.map_size - sizeof(struct sm_ipc_header)) /
                                          sizeof(struct sm_ipc_sample)) {
        if (err && err_len) snprintf(err, err_len, "%s has an invalid capacity", name);
        sm_ipc_shm_detach();
        return -1;
    }
    return 0;
}

void sm_ipc_shm_detach(void) {
    if (g_mapping.map_size > 0 && g_mapping.header != nullptr) {
        munmap(g_mapping.header, g_mapping.map_size);
    }
    if (g_mapping.fd >= 0) {
        ::close(g_mapping.fd);
    }
    g_mapping.fd = -1;
    g_mapping.header = nullptr;
    g_mapping.slots = nullptr;
    g_mapping.map_size = 0;
    g_mapping.name[0] = '\0';
}

int sm_ipc_shm_publish(const struct sm_ipc_sample* sample) {
    if (g_mapping.header == nullptr || sample == nullptr) return -1;

    const uint64_t write_index = __atomic_load_n(&g_mapping.header->write_index, __ATOMIC_ACQUIRE);
    const uint64_t read_index = __atomic_load_n(&g_mapping.header->read_index, __ATOMIC_ACQUIRE);
    const uint64_t capacity = g_mapping.header->capacity;

    int result = 1;
    if (write_index - read_index >= capacity) {
        /* Ring is full: the reader is falling behind.  Overwrite the
         * oldest slot and account for the loss so the dashboard can warn
         * about it instead of silently skipping data. */
        __atomic_fetch_add(&g_mapping.header->dropped, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&g_mapping.header->read_index, read_index + 1, __ATOMIC_RELEASE);
        result = 0;
    }

    struct sm_ipc_sample* slot = slotAt(write_index);
    if (slot == nullptr) return -1;
    slot->sequence = sample->sequence;
    slot->timestamp_ns = sample->timestamp_ns;
    slot->epoch_seconds = sample->epoch_seconds;
    slot->pulse_count = sample->pulse_count;

    g_mapping.header->last_update_ns = realtimeNs();
    __atomic_store_n(&g_mapping.header->write_index, write_index + 1, __ATOMIC_RELEASE);
    return result;
}

int sm_ipc_shm_consume(struct sm_ipc_sample* out) {
    if (g_mapping.header == nullptr || out == nullptr) return 0;

    const uint64_t write_index = __atomic_load_n(&g_mapping.header->write_index, __ATOMIC_ACQUIRE);
    const uint64_t read_index = __atomic_load_n(&g_mapping.header->read_index, __ATOMIC_ACQUIRE);
    if (read_index >= write_index) return 0;

    const struct sm_ipc_sample* slot = slotAt(read_index);
    if (slot == nullptr) return 0;
    out->sequence = slot->sequence;
    out->timestamp_ns = slot->timestamp_ns;
    out->epoch_seconds = slot->epoch_seconds;
    out->pulse_count = slot->pulse_count;
    __atomic_store_n(&g_mapping.header->read_index, read_index + 1, __ATOMIC_RELEASE);
    return 1;
}

uint64_t sm_ipc_shm_available(void) {
    if (g_mapping.header == nullptr) return 0;
    const uint64_t write_index = __atomic_load_n(&g_mapping.header->write_index, __ATOMIC_ACQUIRE);
    const uint64_t read_index = __atomic_load_n(&g_mapping.header->read_index, __ATOMIC_ACQUIRE);
    return write_index > read_index ? write_index - read_index : 0;
}

uint64_t sm_ipc_shm_produced(void) {
    if (g_mapping.header == nullptr) return 0;
    return __atomic_load_n(&g_mapping.header->write_index, __ATOMIC_ACQUIRE);
}

uint64_t sm_ipc_shm_dropped(void) {
    if (g_mapping.header == nullptr) return 0;
    return __atomic_load_n(&g_mapping.header->dropped, __ATOMIC_RELAXED);
}

int sm_ipc_shm_is_attached(void) { return g_mapping.header != nullptr ? 1 : 0; }

const char* sm_ipc_shm_name(void) { return g_mapping.name; }

/* ------------------------------------------------------------------ */
/* POSIX message queue                                                 */
/* ------------------------------------------------------------------ */

int sm_ipc_mq_create(const char* name, long max_messages, long message_bytes, char* err,
                     size_t err_len) {
    if (name == nullptr || name[0] == '\0') {
        if (err && err_len) snprintf(err, err_len, "mq name is empty");
        return -1;
    }
    if (max_messages <= 0) max_messages = 16;
    if (message_bytes <= 0) message_bytes = 256;

    mq_unlink(name); /* start from a known state */
    /* The attributes have to be supplied at creation time: Linux refuses
     * mq_setattr() that lowers mq_maxmsg below the current value, and the
     * default mq_msgsize (8192) would make every mq_timedreceive() with a
     * small buffer fail with EMSGSIZE. */
    struct mq_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.mq_maxmsg = max_messages;
    attr.mq_msgsize = message_bytes;
    g_mqd = mq_open(name, O_CREAT | O_RDWR | O_NONBLOCK, 0666, &attr);
    if (g_mqd == static_cast<mqd_t>(-1)) {
        setError(err, err_len, "mq_open(create)");
        return -1;
    }
    return 0;
}

int sm_ipc_mq_open(const char* name, char* err, size_t err_len) {
    if (name == nullptr || name[0] == '\0') {
        if (err && err_len) snprintf(err, err_len, "mq name is empty");
        return -1;
    }
    sm_ipc_mq_close();
    g_mqd = mq_open(name, O_RDWR | O_NONBLOCK, 0666, nullptr);
    if (g_mqd == static_cast<mqd_t>(-1)) {
        setError(err, err_len, "mq_open");
        return -1;
    }
    return 0;
}

void sm_ipc_mq_close(void) {
    if (g_mqd != static_cast<mqd_t>(-1)) {
        mq_close(g_mqd);
        g_mqd = static_cast<mqd_t>(-1);
    }
}

int sm_ipc_mq_send(const char* payload) {
    if (g_mqd == static_cast<mqd_t>(-1) || payload == nullptr) return -1;
    const size_t length = strlen(payload);
    if (mq_send(g_mqd, payload, length, 0) == 0) return 0;
    if (errno == EAGAIN) return 1; /* queue full: caller decides */
    return -1;
}

int sm_ipc_mq_receive(char* buffer, size_t buffer_len, unsigned timeout_ms) {
    if (g_mqd == static_cast<mqd_t>(-1) || buffer == nullptr || buffer_len == 0) return -1;

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += static_cast<time_t>(timeout_ms / 1000U);
    deadline.tv_nsec += static_cast<long>((timeout_ms % 1000U) * 1000000UL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    ssize_t received = mq_timedreceive(g_mqd, buffer, buffer_len, nullptr, &deadline);
    if (received < 0) {
        if (errno == EAGAIN || errno == ETIMEDOUT) return 0;
        return -1;
    }
    buffer[received] = '\0';
    return static_cast<int>(received);
}

int sm_ipc_mq_is_open(void) { return g_mqd != static_cast<mqd_t>(-1) ? 1 : 0; }

}  // extern "C"
