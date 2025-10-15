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
#include <limits.h>

struct station_sample {
    double signal_dbm;
    double tx_packets;
    double tx_retries;
    double tx_failed;
    double beacon_loss;
    double rx_packets;
    double rx_duplicates;
    double rx_drop_misc;
};

struct metrics {
    double rssi_norm;
    double link_tx_norm;
    double link_rx_norm;
    double link_all_norm;

    double tx_retry_ratio;
    double tx_retry_rate;
    double tx_fail_rate;
    double tx_beacon_rate;
    double tx_packet_rate;

    double rx_retry_ratio;
    double rx_retry_rate;
    double rx_drop_rate;
    double rx_packet_rate;

    bool   valid_rssi;
    bool   valid_link_tx;
    bool   valid_link_rx;
    bool   valid_link_all;

    struct station_sample raw_station;
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

static int resolve_phy_name(const char *iface, char *phy_out, size_t phy_len) {
    char link_path[256];
    snprintf(link_path, sizeof(link_path), "/sys/class/net/%s/phy80211", iface);

    char buf[PATH_MAX];
    ssize_t len = readlink(link_path, buf, sizeof(buf) - 1);
    if (len < 0) {
        fprintf(stderr, "readlink(%s) failed: %s\n", link_path, strerror(errno));
        return -1;
    }
    buf[len] = '\0';
    const char *slash = strrchr(buf, '/');
    if (!slash || slash[1] == '\0') {
        fprintf(stderr, "Unexpected phy path: %s\n", buf);
        return -1;
    }
    strncpy(phy_out, slash + 1, phy_len - 1);
    phy_out[phy_len - 1] = '\0';
    return 0;
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

static int find_first_station(const char *iface, char *mac_out, size_t mac_len) {
    if (!mac_out || mac_len == 0) return -1;

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
                strncpy(mac_out, mac, mac_len - 1);
                mac_out[mac_len - 1] = '\0';
                found = true;
                break;
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
    out->rx_packets = NAN;
    out->rx_duplicates = NAN;
    out->rx_drop_misc = NAN;

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
        } else if (strncmp(trimmed, "rx packets:", 11) == 0) {
            unsigned long long rxp = 0;
            if (sscanf(trimmed + 11, "%llu", &rxp) == 1) {
                out->rx_packets = (double)rxp;
            }
        } else if (strncmp(trimmed, "rx drop misc:", 13) == 0) {
            unsigned long long drop = 0;
            if (sscanf(trimmed + 13, "%llu", &drop) == 1) {
                out->rx_drop_misc = (double)drop;
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

static int fetch_rx_duplicates(const char *phy, const char *iface, const char *mac, double *out_value) {
    if (!phy || !iface || !mac || !out_value) return -1;

    char mac_lower[32];
    normalize_mac(mac, mac_lower, sizeof(mac_lower));

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/kernel/debug/ieee80211/%s/netdev:%s/stations/%s/rx_duplicates",
             phy, iface, mac_lower);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    double total = 0.0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        colon++;
        double value = strtod(colon, NULL);
        if (!isnan(value)) {
            total += value;
        }
    }
    fclose(fp);
    *out_value = total;
    return 0;
}

struct tx_counter_snapshot {
    double tx_packets;
    double tx_retries;
    double tx_failed;
    double beacon_loss;
    bool valid;
};

struct tx_link_metrics {
    double ratio;
    double retries_per_s;
    double fails_per_s;
    double beacon_per_s;
    double packets_per_s;
    double composite;
    bool has_delta;
};

static bool compute_tx_link_metrics(const struct station_sample *current,
                                    const struct tx_counter_snapshot *prev,
                                    double interval_seconds,
                                    struct tx_link_metrics *out) {
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

struct rx_snapshot {
    double rx_packets;
    double rx_duplicates;
    double rx_drop_misc;
};

struct rx_link_metrics {
    double ratio;
    double retry_rate;
    double drop_rate;
    double packets_per_s;
    double composite;
    bool has_delta;
};

static bool compute_rx_link_metrics(const struct rx_snapshot *current,
                                    const struct rx_snapshot *prev,
                                    double interval_seconds,
                                    struct rx_link_metrics *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->ratio = NAN;
    out->retry_rate = NAN;
    out->drop_rate = NAN;
    out->composite = NAN;
    out->has_delta = false;

    if (!prev) {
        out->ratio = 0.0;
        out->retry_rate = 0.0;
        out->drop_rate = 0.0;
        out->composite = 100.0;
        return true;
    }

    double delta_packets = current->rx_packets - prev->rx_packets;
    double delta_duplicates = current->rx_duplicates - prev->rx_duplicates;
    double delta_drop = current->rx_drop_misc - prev->rx_drop_misc;

    if (delta_packets < 0.0 || delta_duplicates < 0.0 || delta_drop < 0.0) {
        out->ratio = 0.0;
        out->retry_rate = 0.0;
        out->drop_rate = 0.0;
        out->composite = 100.0;
        return true;
    }

    if (interval_seconds <= 0.0) interval_seconds = 1.0;

    out->retry_rate = delta_duplicates / interval_seconds;
    out->drop_rate  = delta_drop / interval_seconds;
    out->packets_per_s = delta_packets / interval_seconds;

    double denom = delta_packets;
    if (denom <= 0.0) denom = 1.0;
    double ratio = delta_duplicates / denom;
    if (ratio < 0.0) ratio = 0.0;
    out->ratio = ratio;

    double ratio_score      = 100.0 * (1.0 - clamp(ratio / 0.08, 0.0, 1.0));
    double retry_rate_score = 100.0 * (1.0 - clamp(out->retry_rate / 50.0, 0.0, 1.0));
    double drop_rate_score  = 100.0 * (1.0 - clamp(out->drop_rate  / 5.0,  0.0, 1.0));

    double composite = 0.7 * ratio_score + 0.2 * retry_rate_score + 0.1 * drop_rate_score;
    out->composite = clamp(composite, 0.0, 100.0);
    out->has_delta = true;
    return true;
}

static struct metrics derive_metrics(const struct station_sample *sample,
                                     const struct tx_link_metrics *tx,
                                     const struct rx_link_metrics *rx,
                                     double link_all, bool link_all_valid) {
    struct metrics m = {0};
    m.tx_retry_ratio = NAN;
    m.tx_retry_rate = NAN;
    m.tx_fail_rate = NAN;
    m.tx_beacon_rate = NAN;
    m.tx_packet_rate = NAN;
    m.rx_retry_ratio = NAN;
    m.rx_retry_rate = NAN;
    m.rx_drop_rate = NAN;
    m.link_tx_norm = NAN;
    m.link_rx_norm = NAN;
    m.link_all_norm = NAN;
    m.raw_station = *sample;
    if (!isnan(sample->signal_dbm)) {
        m.rssi_norm = normalize_linear(sample->signal_dbm, -85.0, -20.0);
        m.valid_rssi = true;
    }
    if (tx) {
        m.tx_retry_ratio = tx->ratio;
        m.tx_retry_rate  = tx->retries_per_s;
        m.tx_fail_rate   = tx->fails_per_s;
        m.tx_beacon_rate = tx->beacon_per_s;
        m.tx_packet_rate = tx->packets_per_s;
        if (!isnan(tx->composite)) {
            m.link_tx_norm = tx->composite;
            m.valid_link_tx = true;
        }
    }
    if (rx) {
        m.rx_retry_ratio = rx->ratio;
        m.rx_retry_rate  = rx->retry_rate;
        m.rx_drop_rate   = rx->drop_rate;
        m.rx_packet_rate = rx->packets_per_s;
        if (!isnan(rx->composite)) {
            m.link_rx_norm = rx->composite;
            m.valid_link_rx = true;
        }
    }
    if (link_all_valid && !isnan(link_all)) {
        m.link_all_norm = link_all;
        m.valid_link_all = true;
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
    char payload[512];
    char raw_signal[32];
    char raw_tx_ratio[32], raw_tx_retry_rate[32], raw_tx_fail_rate[32], raw_tx_beacon_rate[32], raw_tx_packet_rate[32];
    char raw_rx_ratio[32], raw_rx_retry_rate[32], raw_rx_drop_rate[32], raw_rx_packet_rate[32];

    format_number(raw_signal, sizeof(raw_signal), m->raw_station.signal_dbm, "%.2f");
    format_number(raw_tx_ratio, sizeof(raw_tx_ratio), m->tx_retry_ratio, "%.6f");
    format_number(raw_tx_retry_rate, sizeof(raw_tx_retry_rate), m->tx_retry_rate, "%.3f");
    format_number(raw_tx_fail_rate, sizeof(raw_tx_fail_rate), m->tx_fail_rate, "%.3f");
    format_number(raw_tx_beacon_rate, sizeof(raw_tx_beacon_rate), m->tx_beacon_rate, "%.3f");
    format_number(raw_tx_packet_rate, sizeof(raw_tx_packet_rate), m->tx_packet_rate, "%.3f");
    format_number(raw_rx_ratio, sizeof(raw_rx_ratio), m->rx_retry_ratio, "%.6f");
    format_number(raw_rx_retry_rate, sizeof(raw_rx_retry_rate), m->rx_retry_rate, "%.3f");
    format_number(raw_rx_drop_rate, sizeof(raw_rx_drop_rate), m->rx_drop_rate, "%.3f");
    format_number(raw_rx_packet_rate, sizeof(raw_rx_packet_rate), m->rx_packet_rate, "%.3f");

    const char *labels[4];
    double values[4];
    size_t count = 0;

    if (m->valid_rssi) {
        labels[count] = "RSSI";
        values[count] = m->rssi_norm;
        count++;
    }
    if (m->valid_link_tx) {
        labels[count] = "Link TX";
        values[count] = m->link_tx_norm;
        count++;
    }
    if (m->valid_link_rx) {
        labels[count] = "Link RX";
        values[count] = m->link_rx_norm;
        count++;
    }
    if (m->valid_link_all) {
        labels[count] = "Link ALL";
        values[count] = m->link_all_norm;
        count++;
    }

    char text_buf[256] = "[";
    char value_buf[256] = "[";
    size_t text_off = 1;
    size_t value_off = 1;
    bool first = true;
    for (size_t i = 0; i < count; i++) {
        if (!first) {
            text_buf[text_off++] = ',';
            value_buf[value_off++] = ',';
        }
        int wt = snprintf(text_buf + text_off, sizeof(text_buf) - text_off,
                          "\"%s\"", labels[i]);
        if (wt < 0 || text_off + (size_t)wt >= sizeof(text_buf)) return -1;
        text_off += (size_t)wt;

        int wv = snprintf(value_buf + value_off, sizeof(value_buf) - value_off,
                          "%.2f", values[i]);
        if (wv < 0 || value_off + (size_t)wv >= sizeof(value_buf)) return -1;
        value_off += (size_t)wv;
        first = false;
    }
    text_buf[text_off++] = ']';
    text_buf[text_off] = '\0';
    value_buf[value_off++] = ']';
    value_buf[value_off] = '\0';

    double link_tx = m->valid_link_tx ? m->link_tx_norm : NAN;
    double link_rx = m->valid_link_rx ? m->link_rx_norm : NAN;
    double link_all = m->valid_link_all ? m->link_all_norm : NAN;

    double link_fallback = !isnan(link_all) ? link_all :
                           !isnan(link_tx) ? link_tx :
                           !isnan(link_rx) ? link_rx : 0.0;

    double rssi_value = m->valid_rssi ? m->rssi_norm : 0.0;
    double link_value = link_fallback;
    double link_tx_value = !isnan(link_tx) ? link_tx : link_value;
    double link_rx_value = !isnan(link_rx) ? link_rx : link_value;
    double link_all_value = !isnan(link_all) ? link_all : link_value;

    char raw_link_tx[32], raw_link_rx[32], raw_link_all[32];
    format_number(raw_link_tx, sizeof(raw_link_tx), link_tx, "%.2f");
    format_number(raw_link_rx, sizeof(raw_link_rx), link_rx, "%.2f");
    format_number(raw_link_all, sizeof(raw_link_all), link_all, "%.2f");

    int len = snprintf(payload, sizeof(payload),
        "{\"rssi\":%.2f,\"link\":%.2f,\"link_tx\":%.2f,\"link_rx\":%.2f,\"link_all\":%.2f,"
        "\"text\":%s,\"value\":%s,"
        "\"raw\":{\"signal\":%s,"
        "\"tx_retry_ratio\":%s,\"tx_retry_rate\":%s,\"tx_fail_rate\":%s,\"tx_beacon_rate\":%s,\"tx_packet_rate\":%s,"
        "\"rx_retry_ratio\":%s,\"rx_retry_rate\":%s,\"rx_drop_rate\":%s,\"rx_packet_rate\":%s,"
        "\"link_tx\":%s,\"link_rx\":%s,\"link_all\":%s}}\n",
        rssi_value,
        link_value,
        link_tx_value,
        link_rx_value,
        link_all_value,
        text_buf,
        value_buf,
        raw_signal,
        raw_tx_ratio,
        raw_tx_retry_rate,
        raw_tx_fail_rate,
        raw_tx_beacon_rate,
        raw_tx_packet_rate,
        raw_rx_ratio,
        raw_rx_retry_rate,
        raw_rx_drop_rate,
        raw_rx_packet_rate,
        raw_link_tx,
        raw_link_rx,
        raw_link_all);
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
        char first_mac[32] = {0};
        int rc = find_first_station(device, first_mac, sizeof(first_mac));
        if (rc != 0) {
            if (rc > 0) {
                fprintf(stderr, "Unable to find a station on %s; specify -m\n", device);
            }
            return 1;
        }
        normalize_mac(first_mac, mac_filter, sizeof(mac_filter));
        printf("Defaulting to station %s\n", first_mac);
        fflush(stdout);
    }

    char phy_name[64];
    if (resolve_phy_name(device, phy_name, sizeof(phy_name)) != 0) {
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

    struct tx_counter_snapshot prev_tx = {.valid = false};
    struct rx_snapshot prev_rx = {0};
    bool prev_rx_valid = false;
    struct rx_link_metrics prev_rx_link = {0};
    bool prev_rx_metrics_valid = false;
    char active_mac[32] = {0};
    struct timespec last_ts = {0};
    bool have_last_ts = false;
    double ema_tx = 100.0;
    double ema_rx = 100.0;
    double ema_all = 100.0;
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
            prev_tx.valid = false;
            prev_rx_valid = false;
            have_last_ts = false;
            fprintf(stderr, "Unable to fetch metrics for %s\n", device);
        } else {
            const char *mac_for_path = matched_mac[0] ? matched_mac : mac_filter;
            if (mac_for_path && mac_for_path[0]) {
                double rx_dup = NAN;
                if (fetch_rx_duplicates(phy_name, device, mac_for_path, &rx_dup) == 0) {
                    sample.rx_duplicates = rx_dup;
                }
            }
            if (isnan(sample.rx_duplicates)) sample.rx_duplicates = 0.0;
            if (isnan(sample.rx_packets)) sample.rx_packets = 0.0;
            if (isnan(sample.rx_drop_misc)) sample.rx_drop_misc = 0.0;

            if (matched_mac[0] && strcmp(matched_mac, active_mac) != 0) {
                strncpy(active_mac, matched_mac, sizeof(active_mac) - 1);
                active_mac[sizeof(active_mac) - 1] = '\0';
                prev_tx.valid = false;
                prev_rx_valid = false;
                ema_tx = ema_rx = ema_all = 100.0;
                have_last_ts = false;
                printf("Tracking station %s on %s\n", active_mac, device);
                fflush(stdout);
            }

    struct tx_link_metrics tx_link = {0};
    bool tx_ready = compute_tx_link_metrics(&sample,
                                            prev_tx.valid ? &prev_tx : NULL,
                                            interval_s, &tx_link);
            if (tx_ready) {
                if (tx_link.has_delta) {
                    ema_tx = ema_alpha * tx_link.composite + (1.0 - ema_alpha) * ema_tx;
                }
                tx_link.composite = ema_tx;
            }

            struct rx_snapshot rx_sample = {
                .rx_packets = sample.rx_packets,
                .rx_duplicates = sample.rx_duplicates,
                .rx_drop_misc = sample.rx_drop_misc,
            };
            struct rx_link_metrics rx_link = prev_rx_metrics_valid ? prev_rx_link : (struct rx_link_metrics){0};
            bool rx_ready = false;
            if (!isnan(sample.rx_packets)) {
                struct rx_link_metrics tmp = {0};
                rx_ready = compute_rx_link_metrics(&rx_sample,
                                                   prev_rx_valid ? &prev_rx : NULL,
                                                   interval_s, &tmp);
                if (rx_ready) {
                    if (tmp.has_delta) {
                        ema_rx = ema_alpha * tmp.composite + (1.0 - ema_alpha) * ema_rx;
                        rx_link = tmp;
                        rx_link.composite = ema_rx;
                        prev_rx_link = rx_link;
                        prev_rx_link.has_delta = true;
                        prev_rx_metrics_valid = true;
                    } else {
                        rx_link.composite = ema_rx;
                    }
                }
            }
            if (!rx_ready && prev_rx_metrics_valid) {
                rx_ready = true;
                rx_link = prev_rx_link;
                rx_link.has_delta = false;
                rx_link.composite = ema_rx;
            }

            double link_all = NAN;
            bool link_all_valid = false;
            double sum = 0.0;
            int contributors = 0;
            if (tx_ready) { sum += ema_tx; contributors++; }
            if (rx_ready) { sum += ema_rx; contributors++; }
            if (contributors > 0) {
                double avg = sum / contributors;
                ema_all = ema_alpha * avg + (1.0 - ema_alpha) * ema_all;
                link_all = ema_all;
                link_all_valid = true;
            } else {
                link_all = ema_all;
                link_all_valid = true;
            }

            struct metrics metrics = derive_metrics(&sample,
                                                    tx_ready ? &tx_link : NULL,
                                                    rx_ready ? &rx_link : NULL,
                                                    link_all, link_all_valid);

            if (!metrics.valid_link_tx && tx_ready) {
                metrics.link_tx_norm = ema_tx;
                metrics.valid_link_tx = true;
            }
            if (!metrics.valid_link_rx && rx_ready) {
                metrics.link_rx_norm = ema_rx;
                metrics.valid_link_rx = true;
            }
            if (!metrics.valid_link_all) {
                metrics.link_all_norm = ema_all;
                metrics.valid_link_all = true;
            }

            if (send_udp_packet(sock, &dest, &metrics) != 0) {
                fprintf(stderr, "Failed to send UDP payload\n");
            }

            if (verbose) {
                double hz = interval_s > 0.0 ? (1.0 / interval_s) : 0.0;
                printf("mac=%s Hz=%.2f rssi=%.1f dBm (norm %.1f) "
                       "link_tx=%.1f link_rx=%.1f link_all=%.1f "
                       "tx_ratio=%.4f tx_retries/s=%.2f tx_fail/s=%.2f tx_beacon/s=%.2f tx_packets/s=%.2f "
                       "rx_ratio=%.4f rx_retries/s=%.2f rx_drop/s=%.2f rx_packets/s=%.2f\n",
                       active_mac[0] ? active_mac : matched_mac,
                       hz,
                       sample.signal_dbm,
                       metrics.valid_rssi ? metrics.rssi_norm : NAN,
                       metrics.valid_link_tx ? metrics.link_tx_norm : NAN,
                       metrics.valid_link_rx ? metrics.link_rx_norm : NAN,
                       metrics.valid_link_all ? metrics.link_all_norm : NAN,
                       tx_ready ? tx_link.ratio : NAN,
                       tx_ready ? tx_link.retries_per_s : NAN,
                       tx_ready ? tx_link.fails_per_s : NAN,
                       tx_ready ? tx_link.beacon_per_s : NAN,
                       tx_ready ? tx_link.packets_per_s : NAN,
                       rx_ready ? rx_link.ratio : NAN,
                       rx_ready ? rx_link.retry_rate : NAN,
                       rx_ready ? rx_link.drop_rate : NAN,
                       rx_ready ? rx_link.packets_per_s : NAN);
                fflush(stdout);
            }

            if (!isnan(sample.tx_packets) && !isnan(sample.tx_retries) &&
                !isnan(sample.tx_failed) && !isnan(sample.beacon_loss)) {
                prev_tx.tx_packets = sample.tx_packets;
                prev_tx.tx_retries = sample.tx_retries;
                prev_tx.tx_failed  = sample.tx_failed;
                prev_tx.beacon_loss = sample.beacon_loss;
                prev_tx.valid = true;
            } else {
                prev_tx.valid = false;
            }

            prev_rx = rx_sample;
            prev_rx_valid = true;

            if (rx_ready) {
                prev_rx_link = rx_link;
                prev_rx_metrics_valid = true;
            } else {
                prev_rx_metrics_valid = prev_rx_metrics_valid && rx_ready;
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
