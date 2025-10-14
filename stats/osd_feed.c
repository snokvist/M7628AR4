#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <math.h>
#include <poll.h>

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-s SOCKET] [-p PORT] [-b ADDR] [-T TTL_MS]\n"
        "  -s, --socket   Path to UNIX DGRAM socket (default: /run/pixelpilot/osd.sock)\n"
        "  -p, --port     UDP port to listen on (default: 5005)\n"
        "  -b, --bind     UDP bind address (default: 0.0.0.0)\n"
        "  -T, --ttl      Include ttl_ms in JSON (default: 0 = omit)\n",
        argv0);
}

static int send_json(int fd, const char *sock_path, const char *json)
{
    size_t len = strlen(json);
    ssize_t sent = send(fd, json, len, 0);
    if (sent < 0) {
        fprintf(stderr, "send() to %s failed: %s\n", sock_path, strerror(errno));
        return -1;
    }
    return 0;
}

struct udp_metrics {
    double rssi;
    double link;
    bool has_rssi;
    bool has_link;
};

struct metric_state {
    double value;
    uint64_t last_update_ms;
    bool seen;
};

struct snapshot_state {
    double value;
    bool present;
};

static bool parse_metric(const char *payload, const char *key, double *out) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *pos = strstr(payload, pattern);
    if (!pos) return false;
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }
    errno = 0;
    char *endptr = NULL;
    double value = strtod(pos, &endptr);
    if (errno != 0 || endptr == pos) {
        return false;
    }
    *out = value;
    return true;
}

static int build_osd_payload(const char *texts[], const double values[],
                             const bool present[], size_t count,
                             int ttl_ms, char *out, size_t out_len) {
    char text_part[256] = "[";
    char value_part[256] = "[";
    size_t text_off = 1;
    size_t value_off = 1;
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        if (!present[i]) continue;
        if (!texts[i]) continue;
        if (!first) {
            if (text_off + 1 >= sizeof(text_part) || value_off + 1 >= sizeof(value_part)) {
                return -1;
            }
            text_part[text_off++] = ',';
            value_part[value_off++] = ',';
        }
        int written_text = snprintf(text_part + text_off, sizeof(text_part) - text_off,
                                    "\"%s\"", texts[i]);
        if (written_text < 0 || text_off + (size_t)written_text >= sizeof(text_part)) {
            return -1;
        }
        text_off += (size_t)written_text;

        int written_value = snprintf(value_part + value_off, sizeof(value_part) - value_off,
                                     "%.2f", values[i]);
        if (written_value < 0 || value_off + (size_t)written_value >= sizeof(value_part)) {
            return -1;
        }
        value_off += (size_t)written_value;
        first = false;
    }

    if (text_off + 1 >= sizeof(text_part) || value_off + 1 >= sizeof(value_part)) {
        return -1;
    }
    text_part[text_off++] = ']';
    text_part[text_off] = '\0';
    value_part[value_off++] = ']';
    value_part[value_off] = '\0';

    if (ttl_ms > 0) {
        return snprintf(out, out_len,
                        "{\"text\":%s,\"value\":%s,\"ttl_ms\":%d}\n",
                        text_part, value_part, ttl_ms);
    }
    return snprintf(out, out_len,
                    "{\"text\":%s,\"value\":%s}\n",
                    text_part, value_part);
}

static int ensure_unix_connection(int *fd, const char *sock_path)
{
    if (*fd >= 0) {
        return 0;
    }

    int new_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (new_fd < 0) {
        fprintf(stderr, "socket(AF_UNIX,SOCK_DGRAM) failed: %s\n", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Socket path too long: %s\n", sock_path);
        close(new_fd);
        return -1;
    }
    strcpy(addr.sun_path, sock_path);

    if (connect(new_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect(%s) failed: %s\n", sock_path, strerror(errno));
        close(new_fd);
        return -1;
    }

    *fd = new_fd;
    fprintf(stdout, "Connected to UNIX socket %s\n", sock_path);
    fflush(stdout);
    return 0;
}

int main(int argc, char **argv)
{
    const char *sock_path = "/run/pixelpilot/osd.sock";
    const char *bind_addr = "0.0.0.0";
    int udp_port = 5005;
    int ttl_ms = 0;

    static struct option long_opts[] = {
        {"socket", required_argument, 0, 's'},
        {"port",   required_argument, 0, 'p'},
        {"bind",   required_argument, 0, 'b'},
        {"ttl",    required_argument, 0, 'T'},
        {"help",   no_argument,       0, 'h'},
        {0,0,0,0}
    };

    for (;;) {
        int opt, idx=0;
        opt = getopt_long(argc, argv, "s:p:b:T:h", long_opts, &idx);
        if (opt == -1) break;
        switch (opt) {
            case 's': sock_path = optarg; break;
            case 'p': udp_port = atoi(optarg); break;
            case 'b': bind_addr = optarg; break;
            case 'T': ttl_ms = atoi(optarg); break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int unix_fd = -1;
    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        fprintf(stderr, "socket(AF_INET,SOCK_DGRAM) failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)udp_port);
    if (strcmp(bind_addr, "*") == 0) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid bind address: %s\n", bind_addr);
        close(udp_fd);
        return 1;
    }

    if (bind(udp_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind() failed: %s\n", strerror(errno));
        close(udp_fd);
        return 1;
    }

    fprintf(stdout, "Listening on %s:%d for UDP metrics\n", bind_addr, udp_port);
    fflush(stdout);

    enum { METRIC_RSSI = 0, METRIC_LINK = 1, METRIC_COUNT = 2 };
    struct metric_state metrics[METRIC_COUNT] = {0};
    struct snapshot_state last_sent[METRIC_COUNT] = {0};
    bool snapshot_valid = false;
    const char *base_labels[METRIC_COUNT] = {"RSSI", "Link"};

    const uint64_t stale_timeout_ms = 5000;
    const uint64_t connect_retry_ms = 1000;
    uint64_t last_connect_attempt_ms = 0;
    uint64_t start_ms = now_ms();
    uint64_t last_data_ms = 0;
    uint64_t last_fallback_send_ms = 0;
    uint64_t last_send_ms = 0;
    uint64_t update_counter = 0;

    char udp_buf[512];
    char json_buf[512];
    while (!g_stop) {
        struct pollfd pfd = {
            .fd = udp_fd,
            .events = POLLIN
        };

        int poll_rc = poll(&pfd, 1, 1000);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "poll() failed: %s\n", strerror(errno));
            break;
        }

        uint64_t now = now_ms();
        bool packet_updated = false;

        if (poll_rc > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = recvfrom(udp_fd, udp_buf, sizeof(udp_buf) - 1, 0, NULL, NULL);
            if (n < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "recvfrom() failed: %s\n", strerror(errno));
            } else {
                udp_buf[n] = '\0';

                bool any_field = false;
                double value;
                if (parse_metric(udp_buf, "rssi", &value)) {
                    metrics[METRIC_RSSI].value = value;
                    metrics[METRIC_RSSI].last_update_ms = now;
                    metrics[METRIC_RSSI].seen = true;
                    any_field = true;
                }
                if (parse_metric(udp_buf, "link", &value)) {
                    metrics[METRIC_LINK].value = value;
                    metrics[METRIC_LINK].last_update_ms = now;
                    metrics[METRIC_LINK].seen = true;
                    any_field = true;
                }
                if (!any_field) {
                    fprintf(stderr, "No usable metrics in payload: %s\n", udp_buf);
                } else {
                    packet_updated = true;
                    last_data_ms = now;
                }
            }
        }

        bool fallback_active = false;
        if (last_data_ms == 0) {
            if (now - start_ms >= stale_timeout_ms) {
                fallback_active = true;
            }
        } else if (now - last_data_ms >= stale_timeout_ms) {
            fallback_active = true;
        }

        bool include_metric[METRIC_COUNT] = {false};
        double current_values[METRIC_COUNT] = {0.0};
        bool any_present = false;
        if (fallback_active) {
            any_present = true;
            for (int i = 0; i < METRIC_COUNT; i++) {
                include_metric[i] = true;
                current_values[i] = 0.0;
            }
        } else {
            for (int i = 0; i < METRIC_COUNT; i++) {
                if (!metrics[i].seen) {
                    continue;
                }
                include_metric[i] = true;
                any_present = true;

                uint64_t age = now - metrics[i].last_update_ms;
                if (metrics[i].last_update_ms > 0 && age <= stale_timeout_ms) {
                    current_values[i] = metrics[i].value;
                } else {
                    current_values[i] = 0.0;
                }
            }
        }

        if (!any_present) {
            continue;
        }

        bool changed = !snapshot_valid;
        for (int i = 0; i < METRIC_COUNT && !changed; i++) {
            if (include_metric[i] != last_sent[i].present) {
                changed = true;
                break;
            }
            if (include_metric[i] && fabs(current_values[i] - last_sent[i].value) > 0.001) {
                changed = true;
                break;
            }
        }

        bool fallback_tick = false;
        if (fallback_active) {
            if (last_fallback_send_ms == 0 || (now - last_fallback_send_ms) >= 1000) {
                fallback_tick = true;
            }
        } else {
            last_fallback_send_ms = 0;
        }

        bool should_send = packet_updated || changed || fallback_tick;
        if (!should_send) {
            continue;
        }

        struct udp_metrics out = {0};
        if (include_metric[METRIC_RSSI]) {
            out.has_rssi = true;
            out.rssi = current_values[METRIC_RSSI];
        }
        if (include_metric[METRIC_LINK]) {
            out.has_link = true;
            out.link = current_values[METRIC_LINK];
        }

        double values_arr[METRIC_COUNT] = {0};
        bool present_arr[METRIC_COUNT] = {false};
        char text_buf[METRIC_COUNT][64];
        const char *text_ptrs[METRIC_COUNT] = {0};

        if (out.has_rssi) {
            present_arr[METRIC_RSSI] = true;
            values_arr[METRIC_RSSI] = out.rssi;
        }
        if (out.has_link) {
            present_arr[METRIC_LINK] = true;
            values_arr[METRIC_LINK] = out.link;
        }

        uint64_t next_count = update_counter + 1;
        double freq_hz = 0.0;
        if (last_send_ms != 0) {
            uint64_t delta_ms = now - last_send_ms;
            if (delta_ms > 0) {
                freq_hz = 1000.0 / (double)delta_ms;
            }
        }

        for (int i = 0; i < METRIC_COUNT; i++) {
            if (!present_arr[i]) continue;
            int written_text = snprintf(text_buf[i], sizeof(text_buf[i]),
                                        "%s #%llu @ %.2f Hz",
                                        base_labels[i],
                                        (unsigned long long)next_count,
                                        freq_hz);
            if (written_text < 0 || (size_t)written_text >= sizeof(text_buf[i])) {
                text_buf[i][sizeof(text_buf[i]) - 1] = '\0';
            }
            text_ptrs[i] = text_buf[i];
        }

        int written = build_osd_payload(text_ptrs, values_arr, present_arr,
                                        METRIC_COUNT, ttl_ms, json_buf, sizeof(json_buf));
        if (written < 0 || (size_t)written >= sizeof(json_buf)) {
            fprintf(stderr, "Failed to build JSON payload\n");
            continue;
        }

        if (unix_fd < 0) {
            if (last_connect_attempt_ms == 0 || (now - last_connect_attempt_ms) >= connect_retry_ms) {
                if (ensure_unix_connection(&unix_fd, sock_path) != 0) {
                    last_connect_attempt_ms = now;
                } else {
                    last_connect_attempt_ms = now;
                }
            }
        }

        if (unix_fd < 0) {
            continue;
        }

        if (send_json(unix_fd, sock_path, json_buf) != 0) {
            close(unix_fd);
            unix_fd = -1;
            last_connect_attempt_ms = now;
            continue;
        }

        last_send_ms = now;
        update_counter = next_count;

        fprintf(stdout, "Forwarded: %s", json_buf);
        fflush(stdout);

        if (fallback_active) {
            last_fallback_send_ms = now;
        }
        for (int i = 0; i < METRIC_COUNT; i++) {
            last_sent[i].present = include_metric[i];
            last_sent[i].value = include_metric[i] ? current_values[i] : 0.0;
        }
        snapshot_valid = true;
    }

    if (unix_fd >= 0) {
        close(unix_fd);
    }
    close(udp_fd);
    return 0;
}
