#include "mqtt.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "util.h"

/* ------------------------------------------------------------------------ */
/* MQTT (3.1.1, QoS 0) client thread                                        */
/* ------------------------------------------------------------------------ */

static pthread_mutex_t mq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mq_cond;
static struct {
    char topic[160];
    char payload[256];
    unsigned seq;                    /* bumped by the UI on every change */
} mq_ch[NCH];
static volatile int mq_connected;
static char status_topic[160];

static void mq_put_str(uint8_t **p, const char *s)
{
    size_t n = strlen(s);
    *(*p)++ = (uint8_t)(n >> 8);
    *(*p)++ = (uint8_t)n;
    memcpy(*p, s, n);
    *p += n;
}

static size_t mq_put_len(uint8_t *p, size_t len)
{
    size_t i = 0;
    do {
        uint8_t d = len % 128;
        len /= 128;
        if (len) d |= 0x80;
        p[i++] = d;
    } while (len);
    return i;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
    while (len) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        buf += n; len -= (size_t)n;
    }
    return 0;
}

static int mq_send_packet(int fd, uint8_t type, const uint8_t *body, size_t len)
{
    uint8_t hdr[5];
    size_t h;
    hdr[0] = type;
    h = 1 + mq_put_len(hdr + 1, len);
    if (send_all(fd, hdr, h) < 0) return -1;
    return len ? send_all(fd, body, len) : 0;
}

static int mq_publish(int fd, const char *topic, const char *payload, int retain)
{
    uint8_t body[512], *p = body;
    size_t tl = strlen(topic), pl = strlen(payload);
    if (tl + pl + 2 > sizeof(body)) return -1;
    mq_put_str(&p, topic);
    memcpy(p, payload, pl);
    p += pl;
    return mq_send_packet(fd, 0x30 | (retain ? 1 : 0), body, (size_t)(p - body));
}

static int tcp_connect(const char *host, int port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[8];
    int fd = -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        syslog(LOG_WARNING, "mqtt: cannot resolve %s", host);
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        struct pollfd pfd;
        int err = 0, one = 1;
        socklen_t el = sizeof(err);
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0 && errno != EINPROGRESS) {
            close(fd); fd = -1; continue;
        }
        pfd.fd = fd; pfd.events = POLLOUT;
        if (poll(&pfd, 1, timeout_ms) != 1 ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
            close(fd); fd = -1; continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        break;
    }
    freeaddrinfo(res);
    return fd;
}

static int read_timeout(int fd, uint8_t *buf, size_t len, int timeout_ms)
{
    size_t got = 0;
    while (got < len) {
        struct pollfd pfd;
        ssize_t n;
        pfd.fd = fd; pfd.events = POLLIN;
        if (poll(&pfd, 1, timeout_ms) != 1) return -1;
        n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

static int mq_connect(void)
{
    uint8_t body[600], *p = body, ack[4] = { 0, 0, 0, 0 };
    uint8_t flags = 0x02; /* clean session */
    int fd;
    const char *cid = cfg.mqtt_client_id;
    char cid_buf[64];

    if (!cid[0]) {
        char hn[48] = "nest";
        gethostname(hn, sizeof(hn) - 1);
        snprintf(cid_buf, sizeof(cid_buf), "nest-picker-%s", hn);
        cid = cid_buf;
    }

    fd = tcp_connect(cfg.mqtt_host, cfg.mqtt_port, 5000);
    if (fd < 0) return -1;

    flags |= 0x04 | 0x20; /* will flag, will retain, will QoS 0 */
    if (cfg.mqtt_user[0]) flags |= 0x80;
    if (cfg.mqtt_user[0] && cfg.mqtt_pass[0]) flags |= 0x40;

    mq_put_str(&p, "MQTT");
    *p++ = 4;            /* protocol level 3.1.1 */
    *p++ = flags;
    *p++ = 0; *p++ = 60; /* keepalive seconds */
    mq_put_str(&p, cid);
    mq_put_str(&p, status_topic);
    mq_put_str(&p, "offline");
    if (flags & 0x80) mq_put_str(&p, cfg.mqtt_user);
    if (flags & 0x40) mq_put_str(&p, cfg.mqtt_pass);

    if (mq_send_packet(fd, 0x10, body, (size_t)(p - body)) < 0 ||
        read_timeout(fd, ack, 4, 5000) < 0 || ack[0] != 0x20 || ack[3] != 0) {
        syslog(LOG_WARNING, "mqtt: connect to %s:%d refused (rc=%d)", cfg.mqtt_host, cfg.mqtt_port, ack[3]);
        close(fd);
        return -1;
    }
    mq_publish(fd, status_topic, "online", 1);
    syslog(LOG_INFO, "mqtt: connected to %s:%d as %s", cfg.mqtt_host, cfg.mqtt_port, cid);
    return fd;
}

static void *mqtt_thread(void *arg)
{
    int fd = -1, backoff = 1000, c;
    unsigned sent_seq[NCH];
    int64_t last_tx = 0, last_rx = 0, last_pub[NCH] = { 0, 0 };
    (void)arg;

    while (running) {
        if (fd < 0) {
            struct timespec ts;
            if (!cfg.mqtt_host[0]) { sleep(1); continue; }
            fd = mq_connect();
            if (fd < 0) {
                clock_gettime(CLOCK_MONOTONIC, &ts);
                ts.tv_sec += backoff / 1000;
                pthread_mutex_lock(&mq_lock);
                pthread_cond_timedwait(&mq_cond, &mq_lock, &ts);
                pthread_mutex_unlock(&mq_lock);
                backoff = backoff * 2 > 30000 ? 30000 : backoff * 2;
                continue;
            }
            backoff = 1000;
            mq_connected = 1;
            last_tx = last_rx = now_ms();
            for (c = 0; c < NCH; c++) sent_seq[c] = mq_ch[c].seq - 1; /* (re)publish on connect */
        }

        {
            int64_t t = now_ms();
            struct pollfd pfd;
            int wait_ms = 50;

            for (c = 0; c < NCH; c++) {
                char topic[160], payload[256];
                unsigned seq;
                pthread_mutex_lock(&mq_lock);
                seq = mq_ch[c].seq;
                copy_str(topic, sizeof(topic), mq_ch[c].topic);
                copy_str(payload, sizeof(payload), mq_ch[c].payload);
                pthread_mutex_unlock(&mq_lock);
                if (seq == sent_seq[c] || !payload[0]) continue;
                if (t - last_pub[c] >= cfg.publish_min_ms) {
                    if (mq_publish(fd, topic, payload, cfg.mqtt_retain) < 0) goto drop;
                    sent_seq[c] = seq;
                    last_pub[c] = last_tx = t;
                } else {
                    int w = (int)(cfg.publish_min_ms - (t - last_pub[c]));
                    if (w < wait_ms) wait_ms = w;
                }
            }
            if (t - last_tx >= 30000) {
                if (mq_send_packet(fd, 0xC0, NULL, 0) < 0) goto drop; /* PINGREQ */
                last_tx = t;
            }
            if (t - last_rx > 90000) {
                syslog(LOG_WARNING, "mqtt: keepalive timeout");
                goto drop;
            }

            if (wait_ms < 1) wait_ms = 1;
            pfd.fd = fd; pfd.events = POLLIN;
            if (poll(&pfd, 1, wait_ms) == 1) {
                uint8_t buf[256];
                ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n <= 0) goto drop;
                last_rx = now_ms();
            }
            continue;
        }
drop:
        syslog(LOG_WARNING, "mqtt: connection lost");
        close(fd);
        fd = -1;
        mq_connected = 0;
    }

    if (fd >= 0) {
        mq_publish(fd, status_topic, "offline", 1);
        mq_send_packet(fd, 0xE0, NULL, 0); /* DISCONNECT */
        close(fd);
    }
    return NULL;
}

void mq_set(int ch, const char *payload)
{
    pthread_mutex_lock(&mq_lock);
    copy_str(mq_ch[ch].payload, sizeof(mq_ch[ch].payload), payload);
    mq_ch[ch].seq++;
    pthread_cond_signal(&mq_cond);
    pthread_mutex_unlock(&mq_lock);
}

static pthread_t mq_tid;

void mqtt_init(const char *prefix)
{
    pthread_condattr_t ca;
    snprintf(status_topic, sizeof(status_topic), "%s/status", prefix);
    snprintf(mq_ch[CH_COLOR].topic, sizeof(mq_ch[CH_COLOR].topic), "%s/color", prefix);
    snprintf(mq_ch[CH_SOUND].topic, sizeof(mq_ch[CH_SOUND].topic), "%s/sound", prefix);
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&mq_cond, &ca);
}

void mqtt_start(void) { pthread_create(&mq_tid, NULL, mqtt_thread, NULL); }

/* Call after `running` has been cleared: wakes the thread so it can say goodbye. */
void mqtt_stop(void)
{
    pthread_mutex_lock(&mq_lock);
    pthread_cond_signal(&mq_cond);
    pthread_mutex_unlock(&mq_lock);
    pthread_join(mq_tid, NULL);
}

int mqtt_connected(void) { return mq_connected; }

const char *mqtt_topic(int ch) { return mq_ch[ch].topic; }
