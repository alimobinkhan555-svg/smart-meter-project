/*
 * sm_ipc_client.c - standalone POSIX IPC helper (pure C, no C++ runtime).
 *
 * Demonstrates that the shared memory ring and the message queue can be
 * used from any process, not just the dashboard:
 *
 *   ./build/sm-ipc-client publish 250        publish 250 pulses into the ring
 *   ./build/sm-ipc-client status             show ring statistics
 *   ./build/sm-ipc-client consume 10         consume 10 samples and print them
 *   ./build/sm-ipc-client cmd "rate=3600"    send a control command
 *   ./build/sm-ipc-client recv               receive pending control commands
 *
 * Build: make ipc-client
 */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ipc_channel.h"

static void usage(const char* program) {
    fprintf(stderr,
            "usage: %s <command> [argument]\n"
            "  publish <count>    publish <count> pulse samples into the ring\n"
            "  status             print shared memory statistics\n"
            "  consume [count]    consume and print samples (default 10)\n"
            "  cmd <text>         send a control command on the message queue\n"
            "  recv [timeout_ms]  print pending control commands\n"
            "options: --shm <name> --mq <name>\n",
            program);
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t realtime_s(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec;
}

int main(int argc, char **argv) {
    const char *shm_name = SM_IPC_DEFAULT_SHM;
    const char *mq_name = SM_IPC_DEFAULT_MQ;
    const char *command = NULL;
    const char *argument = NULL;
    char err[256] = {0};
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
            shm_name = argv[++i];
        } else if (strcmp(argv[i], "--mq") == 0 && i + 1 < argc) {
            mq_name = argv[++i];
        } else if (command == NULL) {
            command = argv[i];
        } else if (argument == NULL) {
            argument = argv[i];
        }
    }

    if (command == NULL) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(command, "publish") == 0) {
        uint64_t count = argument != NULL ? strtoull(argument, NULL, 10) : 100;
        uint64_t index;
        int dropped = 0;

        if (sm_ipc_shm_attach(shm_name, err, sizeof(err)) != 0) {
            fprintf(stderr, "attach failed: %s\n", err);
            fprintf(stderr, "hint: start the producer first:  ./build/app --publish\n");
            return EXIT_FAILURE;
        }

        for (index = 0; index < count; ++index) {
            struct sm_ipc_sample sample;
            sample.sequence = sm_ipc_shm_produced() + index;
            sample.timestamp_ns = monotonic_ns();
            sample.epoch_seconds = realtime_s();
            sample.pulse_count = index;
            if (sm_ipc_shm_publish(&sample) == 0) ++dropped;
        }
        printf("published %" PRIu64 " sample(s), %d overwritten\n", count, dropped);
        printf("ring: %" PRIu64 " produced, %" PRIu64 " pending, %" PRIu64
               " dropped\n",
               sm_ipc_shm_produced(), sm_ipc_shm_available(), sm_ipc_shm_dropped());
        sm_ipc_shm_detach();
        return EXIT_SUCCESS;
    }

    if (strcmp(command, "status") == 0) {
        if (sm_ipc_shm_attach(shm_name, err, sizeof(err)) != 0) {
            fprintf(stderr, "attach failed: %s\n", err);
            return EXIT_FAILURE;
        }
        printf("shm object      : %s\n", sm_ipc_shm_name());
        printf("produced        : %" PRIu64 "\n", sm_ipc_shm_produced());
        printf("pending         : %" PRIu64 "\n", sm_ipc_shm_available());
        printf("dropped         : %" PRIu64 "\n", sm_ipc_shm_dropped());
        sm_ipc_shm_detach();
        return EXIT_SUCCESS;
    }

    if (strcmp(command, "consume") == 0) {
        uint64_t count = argument != NULL ? strtoull(argument, NULL, 10) : 10;
        uint64_t index;

        if (sm_ipc_shm_attach(shm_name, err, sizeof(err)) != 0) {
            fprintf(stderr, "attach failed: %s\n", err);
            return EXIT_FAILURE;
        }
        printf("%-10s %-12s %-14s\n", "sequence", "pulse_count", "timestamp");
        for (index = 0; index < count; ++index) {
            struct sm_ipc_sample sample;
            if (sm_ipc_shm_consume(&sample) != 1) {
                printf("(ring empty)\n");
                break;
            }
            printf("%-10" PRIu64 " %-12" PRIu64 " %" PRIu64 "s\n", sample.sequence,
                   sample.pulse_count, sample.epoch_seconds);
        }
        sm_ipc_shm_detach();
        return EXIT_SUCCESS;
    }

    if (strcmp(command, "cmd") == 0) {
        int rc;
        if (argument == NULL) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        if (sm_ipc_mq_open(mq_name, err, sizeof(err)) != 0) {
            fprintf(stderr, "mq_open failed: %s\n", err);
            return EXIT_FAILURE;
        }
        rc = sm_ipc_mq_send(argument);
        sm_ipc_mq_close();
        if (rc == 0) {
            printf("command '%s' sent\n", argument);
            return EXIT_SUCCESS;
        }
        fprintf(stderr, "send failed: %s\n", rc == 1 ? "queue full" : strerror(errno));
        return EXIT_FAILURE;
    }

    if (strcmp(command, "recv") == 0) {
        unsigned timeout = argument != NULL ? (unsigned)strtoul(argument, NULL, 10) : 1000;
        char buffer[256];

        if (sm_ipc_mq_open(mq_name, err, sizeof(err)) != 0) {
            fprintf(stderr, "mq_open failed: %s\n", err);
            return EXIT_FAILURE;
        }
        while (sm_ipc_mq_receive(buffer, sizeof(buffer), timeout) > 0) {
            printf("command: %s\n", buffer);
            timeout = 0; /* drain whatever is already queued */
        }
        sm_ipc_mq_close();
        return EXIT_SUCCESS;
    }

    usage(argv[0]);
    return EXIT_FAILURE;
}
