/* /dev/parking_alert 수동 검증 및 운영 점검 도구. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "parking_alert.h"

static const char *device_path(void)
{
    const char *configured = getenv("PARKING_ALERT_DEVICE");
    return configured && configured[0] != '\0' ? configured : "/dev/parking_alert";
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s status\n"
            "  %s set <0-31> [event_id]\n"
            "  %s clear <0-31> [event_id]\n"
            "  %s write-set <0-31> [event_id]\n"
            "  %s write-clear <0-31> [event_id]\n"
            "  %s clear-all\n"
            "  %s watch\n",
            program, program, program, program, program, program, program);
}

static int parse_u32(const char *text, __u32 *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT32_MAX)
        return -1;
    *value = (__u32)parsed;
    return 0;
}

static int parse_u64(const char *text, __u64 *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return -1;
    *value = (__u64)parsed;
    return 0;
}

static void print_state(const struct parking_alert_state *state)
{
    printf("version=%u active_mask=0x%08" PRIx32
           " generation=%" PRIu64 " last_operation=%" PRIu32
           " last_slot=",
           (unsigned int)state->version, (uint32_t)state->active_mask,
           (uint64_t)state->generation, (uint32_t)state->last_operation);
    if (state->last_slot == PARKING_ALERT_NO_SLOT)
        printf("none");
    else
        printf("%" PRIu32, (uint32_t)state->last_slot);
    printf(" last_event_id=%" PRIu64 "\n", (uint64_t)state->last_event_id);
}

static int get_state(int fd)
{
    struct parking_alert_state state;

    if (ioctl(fd, PARKING_ALERT_IOC_GET_STATE, &state) < 0) {
        perror("PARKING_ALERT_IOC_GET_STATE");
        return 1;
    }
    print_state(&state);
    return 0;
}

static int update_slot(int fd, unsigned long request, __u16 operation,
                       int argc, char **argv, int use_write)
{
    struct parking_alert_command command = {
        .version = PARKING_ALERT_API_VERSION,
        .operation = operation,
    };

    if (argc < 3 || parse_u32(argv[2], &command.slot_index) != 0 ||
        command.slot_index >= PARKING_ALERT_MAX_SLOTS) {
        fprintf(stderr, "slot은 0~31 정수여야 합니다.\n");
        return 2;
    }
    if (argc >= 4 && parse_u64(argv[3], &command.event_id) != 0) {
        fprintf(stderr, "event_id는 0 이상의 정수여야 합니다.\n");
        return 2;
    }
    if (use_write) {
        ssize_t written = write(fd, &command, sizeof(command));
        if (written < 0) {
            perror("parking_alert write");
            return 1;
        }
        if ((size_t)written != sizeof(command)) {
            fprintf(stderr, "short write: %zd\n", written);
            return 1;
        }
    } else {
        if (ioctl(fd, request, &command) < 0) {
            perror("parking_alert slot update ioctl");
            return 1;
        }
    }
    return get_state(fd);
}

static int watch_states(int fd)
{
    struct pollfd descriptor = {.fd = fd, .events = POLLIN};
    struct parking_alert_state state;
    ssize_t received;

    for (;;) {
        int ready = poll(&descriptor, 1, -1);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            return 1;
        }
        if ((descriptor.revents & POLLIN) == 0) {
            fprintf(stderr, "unexpected poll events: 0x%x\n", descriptor.revents);
            return 1;
        }
        received = read(fd, &state, sizeof(state));
        if (received < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return 1;
        }
        if ((size_t)received != sizeof(state)) {
            fprintf(stderr, "unexpected state size: %zd\n", received);
            return 1;
        }
        print_state(&state);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    int fd;
    int result;

    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    fd = open(device_path(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", device_path(), strerror(errno));
        return 1;
    }

    if (strcmp(argv[1], "status") == 0) {
        result = get_state(fd);
    } else if (strcmp(argv[1], "set") == 0) {
        result = update_slot(fd, PARKING_ALERT_IOC_SET_SLOT,
                             PARKING_ALERT_OP_SET_SLOT, argc, argv, 0);
    } else if (strcmp(argv[1], "clear") == 0) {
        result = update_slot(fd, PARKING_ALERT_IOC_CLEAR_SLOT,
                             PARKING_ALERT_OP_CLEAR_SLOT, argc, argv, 0);
    } else if (strcmp(argv[1], "write-set") == 0) {
        result = update_slot(fd, 0, PARKING_ALERT_OP_SET_SLOT, argc, argv, 1);
    } else if (strcmp(argv[1], "write-clear") == 0) {
        result = update_slot(fd, 0, PARKING_ALERT_OP_CLEAR_SLOT, argc, argv, 1);
    } else if (strcmp(argv[1], "clear-all") == 0) {
        if (ioctl(fd, PARKING_ALERT_IOC_CLEAR_ALL) < 0) {
            perror("PARKING_ALERT_IOC_CLEAR_ALL");
            result = 1;
        } else {
            result = get_state(fd);
        }
    } else if (strcmp(argv[1], "watch") == 0) {
        result = watch_states(fd);
    } else {
        print_usage(argv[0]);
        result = 2;
    }

    close(fd);
    return result;
}
