#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct station_sample {
    double signal_dbm;
    double tx_packets;
    double tx_retries;
    double tx_failed;
    double beacon_loss;
};

struct metrics {
    double rssi_norm;
    double link_norm;
    double retry_ratio;
    double retry_rate;
    double fail_rate;
    double beacon_rate;
    double packet_rate;
    bool   valid_rssi;
    bool   valid_link;
    struct station_sample raw;
};

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [-d DEVICE] [-m MAC] [-L] [-H HOST] [-p PORT] [-i MS] [-c COUNT] [-v]\n"
        "  -d DEVICE   Wireless interface (default: auto-detect managed STA)\n"
        "  -m MAC      Lock onto specific peer MAC address\n"
        "  -L          List associated station MACs and exit\n"
        "  -H HOST     UDP receiver (default: 127.0.0.1)\n"
        "  -p PORT     UDP receiver port (default: 5005)\n"
        "  -i MS       Interval between sends (default: 1000 ms)\n"
        "  -c COUNT    Number of packets to send (default: 0 = infinite)\n"
        "  -v          Verbose logging of raw metrics\n",
        argv0);
}

static double clamp(double value, double lo, double hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static double normalize_linear(double value, double min, double max) {
    if (max <= min) return 0.0;
    double norm = (value - min) / (max - min);
    return clamp(norm * 100.0, 0.0, 100.0);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    *end = '\0';
    return s;
}

static void normalize_mac(const char *src, char *dst, size_t dst_len) {
    if (!dst_len) return;
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dst_len; i++) {
        unsigned char ch = (unsigned char)src[i];
        dst[di++] = (char)tolower(ch);
    }
    dst[di] = '\0';
}

static bool mac_equal(const char *a, const char *b) {
    return a && b && strcasecmp(a, b) == 0;
}

static int detect_default_interface(char *out, size_t out_len) {
    FILE *fp = popen("iw dev", "r");
    if (!fp) {
        fprintf(stderr, "popen(iw dev) failed: %s\n", strerror(errno));
        return -1;
    }

    char line[256];
    char current[64] = {0};
    bool found = false;

    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = trim(line);
        if (strncmp(trimmed, "Interface ", 10) == 0) {
            sscanf(trimmed + 10, "%63s", current);
        } else if (strncmp(trimmed, "type ", 5) == 0 && current[0]) {
            char type[32] = {0};
            sscanf(trimmed + 5, "%31s", type);
            if (strcmp(type, "managed") == 0) {
                strncpy(out, current, out_len - 1);
                out[out_len - 1] = '\0';
                found = true;
                break;
            }
        }
    }

    int status = pclose(fp);
    if (status == -1) {
        fprintf(stderr, "iw dev command failed to close\n");
        return -1;
    }
    if (!found) {
        fprintf(stderr, "No managed interface found via iw dev\n");
        return -1;
    }
    return 0;
}

static int list_stations(const char *iface) {
    char cmd[160];
    int written = snprintf(cmd, sizeof(cmd), "iw dev %s station dump", iface);
    if (written < 0 || (size_t)written >= sizeof(cmd)) {
        fprintf(stderr, "Command overflow\n");
        return -1;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "popen(%s) failed: %s\n", cmd, strerror(errno));
        return -1;
    }

    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = trim(line);
        if (strncmp(trimmed, "Station ", 8) == 0) {
            char mac[32] = {0};
            if (sscanf(trimmed + 8, "%31s", mac) == 1) {
                printf("%s\n", mac);
                found = true;
            }
        }
    }

    int status = pclose(fp);
    if (status == -1) {
        fprintf(stderr, "station dump failed to close\n");
        return -1;
    }
    if (!found) {
        fprintf(stderr, "No stations found on %s\n", iface);
        return 1;
    }
    return 0;
}

static int fetch_station_metrics(const char *iface, const char *target_mac,
                                 struct station_sample *out,
                                 char *matched_mac, size_t matched_len) {
    char cmd[200];
    int written = snprintf(cmd, sizeof(cmd),
                           "iw dev %s station get %s",
                           iface, target_mac ? target_mac : "");
    if (written < 0 || (size_t)written >= sizeof(cmd)) {
        fprintf(stderr, "Command overflow\n");
        return -1;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "popen(%s) failed: %s\n", cmd, strerror(errno));
        return -1;
    }

    char line[256];
    bool found = false;
    memset(out, 0, sizeof(*out));
    out->signal_dbm = NAN;
    out->tx_packets = NAN;
    out->tx_retries = NAN;
    out->tx_failed = NAN;
    out->beacon_loss = NAN;

    while (fgets(line, sizeof(line), fp)) {
        char *trimmed = trim(line);
        if (strncmp(trimmed, "Station ", 8) == 0) {
            char mac[32] = {0};
            if (sscanf(trimmed + 8, "%31s", mac) == 1) {
                if (!target_mac || mac_equal(mac, target_mac)) {
                    if (matched_mac && matched_len) {
                        strncpy(matched_mac, mac, matched_len - 1);
                        matched_mac[matched_len - 1] = '\0';
                    }
                    found = true;
                } else {
                    break;
                }
            }
        } else if (!found) {
            continue;
        } else if (strncmp(trimmed, "signal:", 7) == 0) {
            double sig = NAN;
            if (sscanf(trimmed, "signal: %lf", &sig) == 1) {
                out->signal_dbm = sig;
            }
        } else if (strncmp(trimmed, "tx packets:", 11) == 0) {
            unsigned long long packets = 0;
            if (sscanf(trimmed + 11, "%llu", &packets) == 1) {
                out->tx_packets = (double)packets;
            }
        } else if (strncmp(trimmed, "tx retries:", 11) == 0) {
            unsigned long long retries = 0;
            if (sscanf(trimmed + 11, "%llu", &retries) == 1) {
                out->tx_retries = (double)retries;
            }
        } else if (strncmp(trimmed, "tx failed:", 10) == 0) {
            unsigned long long failed = 0;
            if (sscanf(trimmed + 10, "%llu", &failed) == 1) {
                out->tx_failed = (double)failed;
            }
        } else if (strncmp(trimmed, "beacon loss:", 12) == 0) {
            unsigned long long loss = 0;
            if (sscanf(trimmed + 12, "%llu", &loss) == 1) {
                out->beacon_loss = (double)loss;
            }
        }
    }

    int status = pclose(fp);
    if (status == -1) {
        fprintf(stderr, "station get failed to close\n");
        return -1;
    }
    if (!found) {
        fprintf(stderr, "Station %s not found on %s\n",
                target_mac ? target_mac : "(null)", iface);
        return -1;
    }
    return 0;
}

struct counter_snapshot {
    double tx_packets;
    double tx_retries;
    double tx_failed;
    double beacon_loss;
    bool valid;
};

struct link_metrics {
    double ratio;
    double retries_per_s;
    double fails_per_s;
    double beacon_per_s;
    double packets_per_s;
    double composite;
    bool has_delta;
};

static bool compute_link_metrics(const struct station_sample *current,
                                 const struct counter_snapshot *prev,
                                 double interval_seconds,
                                 struct link_metrics *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->ratio = NAN;
    out->composite = NAN;
    out->has_delta = false;

    if (isnan(current->tx_packets) || isnan(current->tx_retries) ||
        isnan(current->tx_failed) || isnan(current->beacon_loss)) {
        return false;
    }

    if (!prev || !prev->valid) {
        out->ratio = 0.0;
        out->composite = 100.0;
        out->has_delta = false;
        return true;
    }

    double delta_packets = current->tx_packets - prev->tx_packets;
    double delta_retries = current->tx_retries - prev->tx_retries;
    double delta_failed  = current->tx_failed  - prev->tx_failed;
    double delta_beacon  = current->beacon_loss - prev->beacon_loss;

    if (delta_packets < 0.0 || delta_retries < 0.0 ||
        delta_failed < 0.0 || delta_beacon < 0.0) {
        out->ratio = 0.0;
        out->composite = 100.0;
        out->has_delta = false;
        return true;
    }

    if (interval_seconds <= 0.0) interval_seconds = 1.0;

    out->packets_per_s = delta_packets / interval_seconds;
    out->retries_per_s = delta_retries / interval_seconds;
    out->fails_per_s   = delta_failed  / interval_seconds;
    out->beacon_per_s  = delta_beacon  / interval_seconds;

    double denom = delta_packets;
    if (denom <= 0.0) denom = 1.0;
    double weighted_failed = delta_failed * 4.0;
    double ratio = (delta_retries + weighted_failed) / denom;
    if (ratio < 0.0) ratio = 0.0;
    out->ratio = ratio;

    double ratio_score  = 100.0 * (1.0 - clamp(ratio / 0.10, 0.0, 1.0));
    double retry_score  = 100.0 * (1.0 - clamp(out->retries_per_s / 60.0, 0.0, 1.0));
    double fail_score   = 100.0 * (1.0 - clamp(out->fails_per_s   / 3.0,  0.0, 1.0));
    double beacon_score = 100.0 * (1.0 - clamp(out->beacon_per_s  / 1.0,  0.0, 1.0));

    double composite = 0.55 * ratio_score +
                       0.25 * retry_score +
                       0.10 * fail_score +
                       0.10 * beacon_score;

    out->composite = clamp(composite, 0.0, 100.0);
    out->has_delta = true;
    return true;
}

static struct metrics derive_metrics(const struct station_sample *sample,
                                     const struct link_metrics *link) {
    struct metrics m = {0};
    m.raw = *sample;
    if (!isnan(sample->signal_dbm)) {
        m.rssi_norm = normalize_linear(sample->signal_dbm, -85.0, 20.0);
        m.valid_rssi = true;
    }
    if (link) {
        m.retry_ratio = link->ratio;
        m.retry_rate  = link->retries_per_s;
        m.fail_rate   = link->fails_per_s;
        m.beacon_rate = link->beacon_per_s;
        m.packet_rate = link->packets_per_s;
        if (!isnan(link->composite)) {
            m.link_norm = link->composite;
            m.valid_link = true;
        }
    }
    return m;
}

static void format_number(char *buf, size_t len, double value, const char *fmt) {
    if (isnan(value) || isinf(value)) {
        snprintf(buf, len, "null");
    } else {
        snprintf(buf, len, fmt, value);
    }
}

static int send_udp_packet(int sock, const struct sockaddr_in *addr,
                           const struct metrics *m) {
    char payload[400];
    char raw_signal[32], raw_ratio[32], raw_retry_rate[32];
    char raw_fail_rate[32], raw_beacon_rate[32], raw_packet_rate[32];

    format_number(raw_signal, sizeof(raw_signal), m->raw.signal_dbm, "%.2f");
    format_number(raw_ratio, sizeof(raw_ratio), m->retry_ratio, "%.6f");
    format_number(raw_retry_rate, sizeof(raw_retry_rate), m->retry_rate, "%.3f");
    format_number(raw_fail_rate, sizeof(raw_fail_rate), m->fail_rate, "%.3f");
    format_number(raw_beacon_rate, sizeof(raw_beacon_rate), m->beacon_rate, "%.3f");
    format_number(raw_packet_rate, sizeof(raw_packet_rate), m->packet_rate, "%.3f");

    double rssi_value = m->valid_rssi ? m->rssi_norm : 0.0;
    double link_value = m->valid_link ? m->link_norm : 0.0;

    int len = snprintf(payload, sizeof(payload),
        "{\"rssi\":%.2f,\"link\":%.2f,"
        "\"text\":[\"RSSI\",\"Link\"],"
        "\"value\":[%.2f,%.2f],"
        "\"raw\":{\"signal\":%s,\"retry_ratio\":%s,"
        "\"retry_rate\":%s,\"fail_rate\":%s,"
        "\"beacon_rate\":%s,\"packet_rate\":%s}}\n",
        rssi_value,
        link_value,
        rssi_value,
        link_value,
        raw_signal,
        raw_ratio,
        raw_retry_rate,
        raw_fail_rate,
        raw_beacon_rate,
        raw_packet_rate);
    if (len < 0 || (size_t)len >= sizeof(payload)) {
        fprintf(stderr, "Failed to format payload\n");
        return -1;
    }

    ssize_t sent = sendto(sock, payload, len, 0,
                          (const struct sockaddr *)addr, sizeof(*addr));
    if (sent < 0) {
        fprintf(stderr, "sendto failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *device = NULL;
    const char *host = "127.0.0.1";
    int port = 5005;
    int interval_ms = 1000;
    int count = 0;
    int list_only = 0;
    int verbose = 0;
    char mac_filter[32] = {0};

    int opt;
    while ((opt = getopt(argc, argv, "d:H:p:i:c:m:Lvh")) != -1) {
        switch (opt) {
            case 'd': device = optarg; break;
            case 'H': host = optarg; break;
            case 'p': port = atoi(optarg); break;
            case 'i': interval_ms = atoi(optarg); break;
            case 'c': count = atoi(optarg); break;
            case 'm':
                normalize_mac(optarg, mac_filter, sizeof(mac_filter));
                break;
            case 'L': list_only = 1; break;
            case 'v': verbose = 1; break;
            case 'h': usage(argv[0]); return 0;
            default: usage(argv[0]); return 1;
        }
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", port);
        return 1;
    }
    if (interval_ms < 0) interval_ms = 0;

    char detected_device[64] = {0};
    if (!device) {
        if (detect_default_interface(detected_device, sizeof(detected_device)) != 0) {
            fprintf(stderr, "Failed to detect interface; use -d\n");
            return 1;
        }
        device = detected_device;
        printf("Detected interface: %s\n", device);
        fflush(stdout);
    }

    if (list_only) {
        int rc = list_stations(device);
        return rc == 0 ? 0 : 1;
    }

    if (!mac_filter[0]) {
        fprintf(stderr, "A MAC address is required (use -m or -L first)\n");
        return 1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "socket failed: %s\n", strerror(errno));
        return 1;
    }

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
    };
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        fprintf(stderr, "inet_pton failed for host %s\n", host);
        close(sock);
        return 1;
    }

    struct counter_snapshot prev = {.valid = false};
    char active_mac[32] = {0};
    struct timespec last_ts = {0};
    bool have_last_ts = false;
    double ema_link = 100.0;
    const double ema_alpha = 0.4;
    int sent = 0;

    for (;;) {
        struct station_sample sample;
        char matched_mac[32] = {0};

        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        double interval_s;
        if (!have_last_ts) {
            interval_s = (interval_ms > 0) ? (interval_ms / 1000.0) : 1.0;
        } else {
            long sec_diff = now_ts.tv_sec - last_ts.tv_sec;
            long nsec_diff = now_ts.tv_nsec - last_ts.tv_nsec;
            interval_s = (double)sec_diff + (double)nsec_diff / 1e9;
            if (interval_s <= 0.0) {
                interval_s = (interval_ms > 0) ? (interval_ms / 1000.0) : 1.0;
            }
        }
        last_ts = now_ts;
        have_last_ts = true;

        if (fetch_station_metrics(device, mac_filter[0] ? mac_filter : NULL,
                                  &sample, matched_mac, sizeof(matched_mac)) != 0) {
            prev.valid = false;
            have_last_ts = false;
            fprintf(stderr, "Unable to fetch metrics for %s\n", device);
        } else {
            if (matched_mac[0] && strcmp(matched_mac, active_mac) != 0) {
                strncpy(active_mac, matched_mac, sizeof(active_mac) - 1);
                active_mac[sizeof(active_mac) - 1] = '\0';
                prev.valid = false;
                ema_link = 100.0;
                have_last_ts = false;
                printf("Tracking station %s on %s\n", active_mac, device);
                fflush(stdout);
            }

            struct link_metrics link = {0};
            bool link_ready = compute_link_metrics(&sample,
                                                   prev.valid ? &prev : NULL,
                                                   interval_s, &link);

            if (link_ready && link.has_delta) {
                ema_link = ema_alpha * link.composite + (1.0 - ema_alpha) * ema_link;
                link.composite = ema_link;
            }

            struct metrics metrics = derive_metrics(&sample, link_ready ? &link : NULL);
            metrics.link_norm = link_ready ? link.composite : ema_link;
            metrics.valid_link = link_ready;

            if (send_udp_packet(sock, &dest, &metrics) != 0) {
                fprintf(stderr, "Failed to send UDP payload\n");
            }

            if (verbose) {
                double hz = interval_s > 0.0 ? (1.0 / interval_s) : 0.0;
                printf("mac=%s Hz=%.2f rssi=%.1f dBm (norm %.1f) "
                       "link=%.1f (ratio=%.4f, retries/s=%.2f, fail/s=%.2f, "
                       "beacon/s=%.2f, packets/s=%.2f)\n",
                       active_mac[0] ? active_mac : matched_mac,
                       hz,
                       sample.signal_dbm,
                       metrics.valid_rssi ? metrics.rssi_norm : NAN,
                       metrics.valid_link ? metrics.link_norm : NAN,
                       link.has_delta ? link.ratio : NAN,
                       link.has_delta ? link.retries_per_s : NAN,
                       link.has_delta ? link.fails_per_s : NAN,
                       link.has_delta ? link.beacon_per_s : NAN,
                       link.has_delta ? link.packets_per_s : NAN);
                fflush(stdout);
            }

            if (!isnan(sample.tx_packets) && !isnan(sample.tx_retries) &&
                !isnan(sample.tx_failed) && !isnan(sample.beacon_loss)) {
                prev.tx_packets = sample.tx_packets;
                prev.tx_retries = sample.tx_retries;
                prev.tx_failed  = sample.tx_failed;
                prev.beacon_loss = sample.beacon_loss;
                prev.valid = true;
            } else {
                prev.valid = false;
            }
        }

        sent++;
        if (count > 0 && sent >= count) break;
        if (interval_ms <= 0) break;

        struct timespec ts = {
            .tv_sec = interval_ms / 1000,
            .tv_nsec = (interval_ms % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
    }

    close(sock);
    return 0;
}
