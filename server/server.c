/*
 * voicechat-server.c
 *
 * Lightweight voice chat relay server.
 *  - TCP for signaling (JSON, newline-delimited)
 *  - UDP for audio relay + P2P hole-punch coordination
 *  - epoll-based I/O (Linux) / kqueue (BSD)
 *  - SO_REUSEPORT + SO_BUSY_POLL (Linux) + recvmmsg for low-latency UDP
 *
 * Build:
 *   gcc -O2 -o voicechat-server server.c -lpthread
 */

/* Do NOT define _POSIX_C_SOURCE here: on FreeBSD that sets __BSD_VISIBLE=0
 * which hides recvmmsg / struct mmsghdr in <sys/socket.h>.
 * Linux builds get -D_GNU_SOURCE from CMake (which implies _POSIX_C_SOURCE
 * 200809 and more); FreeBSD's permissive default exposes BSD extensions. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>

#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>

/* ── I/O multiplexing: epoll (Linux) or kqueue (BSD) ────────────────────── */
#ifdef __linux__
#  include <sys/epoll.h>
   typedef struct epoll_event ev_t;
#  define EV_PTR(e)        ((e).data.ptr)
#  define EV_DISCONNECT(e) ((e).events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
#  define EV_READ(e)       ((e).events & EPOLLIN)
#else
#  include <sys/event.h>
   typedef struct kevent ev_t;
#  define EV_PTR(e)        ((void *)(e).udata)
#  define EV_DISCONNECT(e) ((e).flags & EV_EOF)
#  define EV_READ(e)       ((e).filter == EVFILT_READ)
#endif

#include "../common/protocol.h"

/* ── Types ─────────────────────────────────────────────────────────────── */

typedef struct {
    int      used;
    char     name[VC_MAX_ROOM_NAME];
    uint16_t id;
    int      client_count;
} room_t;

typedef struct {
    int      used;
    uint32_t id;
    char     username[VC_MAX_USERNAME];

    /* Signaling (TCP) */
    int      tcp_fd;
    char     tcp_buf[4096];
    int      tcp_buf_len;

    /* Signaling TCP peer address (populated on accept) */
    struct in_addr tcp_peer_addr;

    /* Audio (UDP) */
    struct sockaddr_in udp_addr;
    int      udp_registered;

    uint16_t room_id;
    int      in_room;

    time_t   last_seen;
    uint16_t last_seq;
} client_t;

/* ── Globals ────────────────────────────────────────────────────────────── */

static room_t   g_rooms[VC_MAX_ROOMS];
static client_t g_clients[VC_MAX_CLIENTS];
static int      g_udp_fd   = -1;
static int      g_tcp_fd   = -1;
static int      g_ev_fd    = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    g_running = 1;
static uint32_t g_next_client_id = 1;

/* ── Utility ────────────────────────────────────────────────────────────── */

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_tcp_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/* ── Room management ────────────────────────────────────────────────────── */

static room_t *room_find(const char *name) {
    for (int i = 0; i < VC_MAX_ROOMS; i++)
        if (g_rooms[i].used && strcmp(g_rooms[i].name, name) == 0)
            return &g_rooms[i];
    return NULL;
}

static room_t *room_find_by_id(uint16_t id) {
    for (int i = 0; i < VC_MAX_ROOMS; i++)
        if (g_rooms[i].used && g_rooms[i].id == id)
            return &g_rooms[i];
    return NULL;
}

static room_t *room_create(const char *name) {
    for (int i = 0; i < VC_MAX_ROOMS; i++) {
        if (!g_rooms[i].used) {
            g_rooms[i].used = 1;
            g_rooms[i].id   = (uint16_t)(i + 1);
            g_rooms[i].client_count = 0;
            snprintf(g_rooms[i].name, VC_MAX_ROOM_NAME, "%s", name);
            return &g_rooms[i];
        }
    }
    return NULL;
}

static void room_maybe_destroy(room_t *r) {
    if (r->client_count <= 0) {
        printf("[room] destroyed '%s'\n", r->name);
        memset(r, 0, sizeof(*r));
    }
}

/* ── Event-poll helpers (epoll on Linux, kqueue on BSD) ─────────────────── */

#ifdef __linux__
static void ev_add(int fd, uint32_t mask, void *ptr) {
    struct epoll_event ev = { .events = mask, .data.ptr = ptr };
    epoll_ctl(g_ev_fd, EPOLL_CTL_ADD, fd, &ev);
}
static void ev_del(int fd) {
    epoll_ctl(g_ev_fd, EPOLL_CTL_DEL, fd, NULL);
}
#else
static void ev_add(int fd, short filter, void *ptr) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, ptr);
    kevent(g_ev_fd, &ev, 1, NULL, 0, NULL);
}
static void ev_del(int fd) {
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(g_ev_fd, &ev, 1, NULL, 0, NULL);
}
#endif

/* ── Client management ──────────────────────────────────────────────────── */

static client_t *client_alloc(int tcp_fd) {
    for (int i = 0; i < VC_MAX_CLIENTS; i++) {
        if (!g_clients[i].used) {
            memset(&g_clients[i], 0, sizeof(client_t));
            g_clients[i].used      = 1;
            g_clients[i].tcp_fd    = tcp_fd;
            g_clients[i].id        = g_next_client_id++;
            g_clients[i].last_seen = time(NULL);
            return &g_clients[i];
        }
    }
    return NULL;
}

static client_t *client_find_by_id(uint32_t id) {
    for (int i = 0; i < VC_MAX_CLIENTS; i++)
        if (g_clients[i].used && g_clients[i].id == id)
            return &g_clients[i];
    return NULL;
}

static void client_free(client_t *c) {
    if (!c || !c->used) return;
    if (c->tcp_fd >= 0) {
        ev_del(c->tcp_fd);
        close(c->tcp_fd);
        c->tcp_fd = -1;
    }
    c->used = 0;
}

/* ── TCP send helpers ───────────────────────────────────────────────────── */

static void tcp_send(client_t *c, const char *msg) {
    if (!c || c->tcp_fd < 0) return;
    size_t len   = strlen(msg);
    size_t sent  = 0;
    while (sent < len) {
        ssize_t n = send(c->tcp_fd, msg + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            fprintf(stderr, "[tcp] send error client %u: %s\n",
                    c->id, strerror(errno));
            break;
        }
        sent += (size_t)n;
    }
}

static void room_broadcast_tcp(uint16_t room_id, const char *msg,
                                uint32_t exclude_id) {
    for (int i = 0; i < VC_MAX_CLIENTS; i++) {
        client_t *c = &g_clients[i];
        if (c->used && c->in_room && c->room_id == room_id
                && c->id != exclude_id) {
            tcp_send(c, msg);
        }
    }
}

/* ── P2P: notify peers of a client's UDP endpoint ───────────────────────── *
 * Called (under g_lock) whenever a client's UDP address is freshly set.    *
 * Sends peer_addr to all other room members, and sends their addresses back  *
 * to the registering client so both sides can punch through simultaneously. */
static void p2p_announce(client_t *c) {
    if (!c->udp_registered || !c->in_room) return;

    /* If the UDP source is loopback (client and server on the same host),
     * fall back to the TCP connection's peer address so remote peers get
     * a reachable IP instead of 127.0.0.1. */
    struct in_addr announce_ip = c->udp_addr.sin_addr;
    if (announce_ip.s_addr == htonl(INADDR_LOOPBACK))
        announce_ip = c->tcp_peer_addr;

    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &announce_ip, addr_str, sizeof(addr_str));
    uint16_t port = ntohs(c->udp_addr.sin_port);

    /* Tell every other room member about c's endpoint */
    char msg_to_peers[256];
    snprintf(msg_to_peers, sizeof(msg_to_peers),
             "{\"op\":\"peer_addr\",\"id\":%u,\"addr\":\"%s\",\"port\":%u}\n",
             c->id, addr_str, port);

    /* Tell c about every other room member's registered endpoint */
    for (int i = 0; i < VC_MAX_CLIENTS; i++) {
        client_t *p = &g_clients[i];
        if (!p->used || !p->in_room || p->id == c->id) continue;
        if (p->room_id != c->room_id) continue;

        /* Tell p about c */
        tcp_send(p, msg_to_peers);

        /* Tell c about p (if p has registered UDP) */
        if (p->udp_registered) {
            struct in_addr p_ip = p->udp_addr.sin_addr;
            if (p_ip.s_addr == htonl(INADDR_LOOPBACK))
                p_ip = p->tcp_peer_addr;
            char addr_p[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &p_ip, addr_p, sizeof(addr_p));
            uint16_t port_p = ntohs(p->udp_addr.sin_port);
            char msg_to_c[256];
            snprintf(msg_to_c, sizeof(msg_to_c),
                     "{\"op\":\"peer_addr\",\"id\":%u,\"addr\":\"%s\",\"port\":%u}\n",
                     p->id, addr_p, port_p);
            tcp_send(c, msg_to_c);
        }
    }
}

/* ── Signaling: handle one JSON message from client ────────────────────── */

static void handle_join(client_t *c, const char *room_name,
                        const char *username) {
    if (c->in_room) {
        tcp_send(c, "{\"op\":\"error\",\"msg\":\"already in room\"}\n");
        return;
    }

    snprintf(c->username, VC_MAX_USERNAME, "%s", username);

    room_t *r = room_find(room_name);
    if (!r) r = room_create(room_name);
    if (!r) {
        tcp_send(c, "{\"op\":\"error\",\"msg\":\"server full\"}\n");
        return;
    }

    c->room_id = r->id;
    c->in_room = 1;
    r->client_count++;

    printf("[room] '%s' joined by %s (id=%u)\n", room_name, username, c->id);

    /* Build ok response with peer list */
    char peers_msg[VC_MAX_CLIENTS * 64];
    int  off = 0;
    off += snprintf(peers_msg + off, sizeof(peers_msg) - off,
                    "{\"op\":\"ok\",\"client_id\":%u,\"room_id\":%u,"
                    "\"peers\":[", c->id, r->id);
    int first = 1;
    for (int i = 0; i < VC_MAX_CLIENTS; i++) {
        client_t *p = &g_clients[i];
        if (p->used && p->in_room && p->room_id == r->id && p->id != c->id) {
            off += snprintf(peers_msg + off, sizeof(peers_msg) - off,
                            "%s{\"id\":%u,\"name\":\"%s\"}",
                            first ? "" : ",", p->id, p->username);
            first = 0;
        }
    }
    off += snprintf(peers_msg + off, sizeof(peers_msg) - off, "]}\n");
    tcp_send(c, peers_msg);

    /* Notify existing peers of the new joiner */
    char joined_msg[256];
    snprintf(joined_msg, sizeof(joined_msg),
             "{\"op\":\"joined\",\"id\":%u,\"name\":\"%s\"}\n",
             c->id, c->username);
    room_broadcast_tcp(r->id, joined_msg, c->id);

    /* If the new client already has UDP registered, announce P2P endpoints */
    if (c->udp_registered) p2p_announce(c);
}

static void handle_leave(client_t *c) {
    if (!c->in_room) return;

    room_t *r = room_find_by_id(c->room_id);
    printf("[room] '%s' left by %s\n", r ? r->name : "?", c->username);

    char left_msg[128];
    snprintf(left_msg, sizeof(left_msg),
             "{\"op\":\"left\",\"id\":%u}\n", c->id);
    room_broadcast_tcp(c->room_id, left_msg, c->id);

    if (r) { r->client_count--; room_maybe_destroy(r); }
    c->in_room = 0;
    c->room_id = 0;
    tcp_send(c, "{\"op\":\"ok\"}\n");
}

static void handle_list(client_t *c) {
    char msg[4096];
    int  off = 0;
    off += snprintf(msg + off, sizeof(msg) - off,
                    "{\"op\":\"rooms\",\"rooms\":[");
    int first = 1;
    for (int i = 0; i < VC_MAX_ROOMS; i++) {
        if (g_rooms[i].used) {
            off += snprintf(msg + off, sizeof(msg) - off,
                            "%s{\"name\":\"%s\",\"count\":%d}",
                            first ? "" : ",",
                            g_rooms[i].name, g_rooms[i].client_count);
            first = 0;
        }
    }
    off += snprintf(msg + off, sizeof(msg) - off, "]}\n");
    tcp_send(c, msg);
}

static int json_get_str(const char *json, const char *key,
                        char *out, int out_len) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return 0;
    p += strlen(search);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

static void dispatch_message(client_t *c, const char *msg) {
    char op[32] = {0};
    if (!json_get_str(msg, "op", op, sizeof(op))) return;

    if (strcmp(op, "join") == 0) {
        char room[VC_MAX_ROOM_NAME] = {0};
        char user[VC_MAX_USERNAME]  = {0};
        json_get_str(msg, "room",     room, sizeof(room));
        json_get_str(msg, "username", user, sizeof(user));
        if (!room[0]) strncpy(room, "general", sizeof(room) - 1);
        if (!user[0]) snprintf(user, sizeof(user), "user%u", c->id);
        handle_join(c, room, user);
    } else if (strcmp(op, "leave") == 0) {
        handle_leave(c);
    } else if (strcmp(op, "list") == 0) {
        handle_list(c);
    } else if (strcmp(op, "ping") == 0) {
        tcp_send(c, "{\"op\":\"pong\"}\n");
    }
}

/* ── UDP relay (with recvmmsg for batched receive) ───────────────────────── */

#define UDP_BATCH 32   /* max packets to receive in one recvmmsg call */

static void udp_relay_packet(const uint8_t *buf, int len,
                             const struct sockaddr_in *src) {
    if (len < (int)VC_HEADER_SIZE) return;

    const vc_packet_header_t *hdr = (const vc_packet_header_t *)buf;
    if (hdr->magic != VC_MAGIC) return;

    pthread_mutex_lock(&g_lock);

    if (hdr->type == PKT_REGISTER || hdr->type == PKT_KEEPALIVE) {
        client_t *c = client_find_by_id(hdr->client_id);
        if (c) {
            int was_registered = c->udp_registered;
            c->udp_addr       = *src;
            c->udp_registered = 1;
            c->last_seen      = time(NULL);

            if (hdr->type == PKT_REGISTER) {
                /* ACK */
                uint8_t ack[VC_HEADER_SIZE];
                vc_packet_header_t *a = (vc_packet_header_t *)ack;
                a->magic = VC_MAGIC; a->type = PKT_REGISTER_ACK;
                a->flags = 0; a->seq = hdr->seq;
                a->client_id = hdr->client_id; a->room_id = hdr->room_id;
                a->payload_len = 0;
                sendto(g_udp_fd, ack, VC_HEADER_SIZE, 0,
                       (struct sockaddr *)src, sizeof(*src));

                /* P2P: tell everyone in the room about this client's endpoint,
                 * and tell this client about everyone else.
                 * Only on first registration or if address changed. */
                if (!was_registered) {
                    printf("[udp] REGISTER client %u @ %s:%d\n",
                           hdr->client_id,
                           inet_ntoa(src->sin_addr), ntohs(src->sin_port));
                    p2p_announce(c);
                }
            }
        }
        pthread_mutex_unlock(&g_lock);
        return;
    }

    if (hdr->type != PKT_AUDIO) {
        pthread_mutex_unlock(&g_lock);
        return;
    }

    /* Relay audio to all other registered clients in the same room */
    uint16_t room_id   = hdr->room_id;
    uint32_t sender_id = hdr->client_id;

    /* Update last_seen for the sender */
    client_t *sender = client_find_by_id(sender_id);
    if (sender) sender->last_seen = time(NULL);

    int relayed = 0;
    for (int i = 0; i < VC_MAX_CLIENTS; i++) {
        client_t *c = &g_clients[i];
        if (!c->used || !c->in_room || !c->udp_registered) continue;
        if (c->room_id != room_id)   continue;
        if (c->id      == sender_id) continue;

        sendto(g_udp_fd, buf, len, 0,
               (struct sockaddr *)&c->udp_addr, sizeof(c->udp_addr));
        relayed++;
    }
    (void)relayed;

    pthread_mutex_unlock(&g_lock);
}

/* ── Housekeeping thread ────────────────────────────────────────────────── */

static void *housekeeping_thread(void *arg) {
    (void)arg;
    while (g_running) {
        sleep(15);
        if (!g_running) break;
        time_t now = time(NULL);
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < VC_MAX_CLIENTS; i++) {
            client_t *c = &g_clients[i];
            if (!c->used) continue;
            if (c->udp_registered && now - c->last_seen > 30)
                c->udp_registered = 0;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* ── Main loop ──────────────────────────────────────────────────────────── */

static void handle_sigint(int s) { (void)s; g_running = 0; }

/* Sentinel pointers to distinguish UDP/TCP listen in epoll data */
static const int UDP_SENTINEL = 0;
static const int TCP_SENTINEL = 1;

int main(void) {
    signal(SIGINT,  handle_sigint);
    signal(SIGTERM, handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    /* ── UDP socket ──────────────────────────────────────────────── */
    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) { perror("socket(udp)"); return 1; }

    /* SO_REUSEPORT: kernel load-balances across multiple threads if needed */
    int opt = 1;
    setsockopt(g_udp_fd, SOL_SOCKET, SO_REUSEPORT,  &opt, sizeof(opt));
    setsockopt(g_udp_fd, SOL_SOCKET, SO_REUSEADDR,  &opt, sizeof(opt));

    /* SO_BUSY_POLL: spin up to 50µs before sleeping — Linux only */
#ifdef SO_BUSY_POLL
    int busy_us = 50;
    setsockopt(g_udp_fd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
#endif

    /* Large socket buffers — prevent drops under burst load */
    int bufsize = 4 * 1024 * 1024;
    setsockopt(g_udp_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(g_udp_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    struct sockaddr_in udp_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(VC_UDP_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(g_udp_fd, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
        perror("bind(udp)"); return 1;
    }
    set_nonblocking(g_udp_fd);
    printf("[server] UDP listening on :%d\n", VC_UDP_PORT);

    /* ── TCP socket ──────────────────────────────────────────────── */
    g_tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(g_tcp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(g_tcp_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in tcp_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(VC_TCP_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };
    if (bind(g_tcp_fd, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
        perror("bind(tcp)"); return 1;
    }
    listen(g_tcp_fd, 128);
    set_nonblocking(g_tcp_fd);
    printf("[server] TCP listening on :%d\n", VC_TCP_PORT);

    /* ── event multiplexor ───────────────────────────────────────── */
#ifdef __linux__
    g_ev_fd = epoll_create1(EPOLL_CLOEXEC);
    if (g_ev_fd < 0) { perror("epoll_create1"); return 1; }
    ev_add(g_udp_fd, EPOLLIN | EPOLLET, (void *)&UDP_SENTINEL);
    ev_add(g_tcp_fd, EPOLLIN,           (void *)&TCP_SENTINEL);
#else
    g_ev_fd = kqueue();
    if (g_ev_fd < 0) { perror("kqueue"); return 1; }
    ev_add(g_udp_fd, EVFILT_READ, (void *)&UDP_SENTINEL);
    ev_add(g_tcp_fd, EVFILT_READ, (void *)&TCP_SENTINEL);
#endif

    /* ── Housekeeping thread ─────────────────────────────────────── */
    pthread_t hk;
    pthread_create(&hk, NULL, housekeeping_thread, NULL);
    pthread_detach(hk);

    /* ── recvmmsg buffers ────────────────────────────────────────── */
    uint8_t            udp_bufs[UDP_BATCH][VC_MAX_PACKET];
    struct iovec       udp_iov[UDP_BATCH];
    struct sockaddr_in udp_src[UDP_BATCH];
    struct mmsghdr     udp_msgs[UDP_BATCH];
    memset(udp_msgs, 0, sizeof(udp_msgs));
    for (int i = 0; i < UDP_BATCH; i++) {
        udp_iov[i].iov_base          = udp_bufs[i];
        udp_iov[i].iov_len           = VC_MAX_PACKET;
        udp_msgs[i].msg_hdr.msg_iov        = &udp_iov[i];
        udp_msgs[i].msg_hdr.msg_iovlen     = 1;
        udp_msgs[i].msg_hdr.msg_name       = &udp_src[i];
        udp_msgs[i].msg_hdr.msg_namelen    = sizeof(udp_src[i]);
    }

    /* ── Event loop ──────────────────────────────────────────────── */
#define MAX_EVENTS 64
    ev_t events[MAX_EVENTS];

    while (g_running) {
#ifdef __linux__
        int n = epoll_wait(g_ev_fd, events, MAX_EVENTS, 1000);
#else
        struct timespec ts = {1, 0};
        int n = kevent(g_ev_fd, NULL, 0, events, MAX_EVENTS, &ts);
#endif
        if (n < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < n; i++) {
            void *ptr = EV_PTR(events[i]);

            if (ptr == &UDP_SENTINEL) {
                /* Drain all pending UDP packets with recvmmsg */
                while (1) {
                    /* Reset name lengths before each call */
                    for (int j = 0; j < UDP_BATCH; j++)
                        udp_msgs[j].msg_hdr.msg_namelen = sizeof(udp_src[j]);

                    int got = recvmmsg(g_udp_fd, udp_msgs, UDP_BATCH,
                                       MSG_DONTWAIT, NULL);
                    if (got <= 0) break;
                    for (int j = 0; j < got; j++)
                        udp_relay_packet(udp_bufs[j],
                                         (int)udp_msgs[j].msg_len,
                                         &udp_src[j]);
                }
                continue;
            }

            if (ptr == &TCP_SENTINEL) {
                /* Accept new connections */
                while (1) {
                    struct sockaddr_in ca;
                    socklen_t cal = sizeof(ca);
                    int cfd = accept4(g_tcp_fd, (struct sockaddr *)&ca,
                                      &cal, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
                    set_tcp_nodelay(cfd);
                    pthread_mutex_lock(&g_lock);
                    client_t *c = client_alloc(cfd);
                    if (c) c->tcp_peer_addr = ca.sin_addr;
                    pthread_mutex_unlock(&g_lock);
                    if (!c) {
                        close(cfd);
                        continue;
                    }
                    printf("[tcp] new client %u from %s\n",
                           c->id, inet_ntoa(ca.sin_addr));
#ifdef __linux__
                    ev_add(cfd, EPOLLIN | EPOLLET | EPOLLRDHUP, c);
#else
                    ev_add(cfd, EVFILT_READ, c);
#endif
                    char greet[128];
                    snprintf(greet, sizeof(greet),
                             "{\"op\":\"hello\",\"client_id\":%u,"
                             "\"udp_port\":%d}\n",
                             c->id, VC_UDP_PORT);
                    tcp_send(c, greet);
                }
                continue;
            }

            /* Must be a client TCP fd */
            client_t *c = (client_t *)ptr;
            if (!c || !c->used) continue;

            if (EV_DISCONNECT(events[i])) {
                pthread_mutex_lock(&g_lock);
                handle_leave(c);
                client_free(c);
                pthread_mutex_unlock(&g_lock);
                continue;
            }

            if (EV_READ(events[i])) {
                /* Drain all available data (edge-triggered) */
                while (1) {
                    int space = (int)sizeof(c->tcp_buf) - c->tcp_buf_len - 1;
                    if (space <= 0) { c->tcp_buf_len = 0; break; }
                    ssize_t nr = recv(c->tcp_fd,
                                      c->tcp_buf + c->tcp_buf_len, space, 0);
                    if (nr < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        /* Error */
                        pthread_mutex_lock(&g_lock);
                        handle_leave(c);
                        client_free(c);
                        pthread_mutex_unlock(&g_lock);
                        break;
                    }
                    if (nr == 0) {
                        pthread_mutex_lock(&g_lock);
                        handle_leave(c);
                        client_free(c);
                        pthread_mutex_unlock(&g_lock);
                        break;
                    }
                    c->tcp_buf_len += (int)nr;
                    c->tcp_buf[c->tcp_buf_len] = '\0';

                    char *start = c->tcp_buf;
                    char *nl;
                    while ((nl = strchr(start, '\n')) != NULL) {
                        *nl = '\0';
                        if (nl > start) {
                            pthread_mutex_lock(&g_lock);
                            dispatch_message(c, start);
                            pthread_mutex_unlock(&g_lock);
                        }
                        start = nl + 1;
                    }
                    int remaining = (int)(c->tcp_buf + c->tcp_buf_len - start);
                    if (remaining > 0 && start != c->tcp_buf)
                        memmove(c->tcp_buf, start, remaining);
                    c->tcp_buf_len = remaining;
                }
            }
        }
    }

    printf("[server] shutting down\n");
    close(g_ev_fd);
    close(g_udp_fd);
    close(g_tcp_fd);
    return 0;
}
