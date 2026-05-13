// L0

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <poll.h>

#include "node.h"

#define MAX_PATH 1024
#define CACHE_PATH "./astrojit/l1_so_store"

#define USE_LOG 0
#define LOG(fmt, ...) if (USE_LOG) ajlog(__LINE__, (fmt), ##__VA_ARGS__)
#define _LOG(fmt, ...) ajlog(__LINE__, (fmt), ##__VA_ARGS__)

static inline void
ajlog(int line, const char *fmt, ...)
{
    fprintf(stderr, "[AJLOG@%d] ", line);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

static void *
astro_jit_dlopen(const char *name)
{
    char buff[MAX_PATH] = "";
    snprintf(buff, MAX_PATH, "%s/%s", CACHE_PATH, name);
    return dlopen(buff, RTLD_LAZY);
}

static void *
astro_jit_so_all(void)
{
    static bool checked = false;
    static void *all;

    if (!checked) {
        all = astro_jit_dlopen("all.so");
        checked = true;
    }

    return all;
}

static void *
astro_jit_search_all(const char *name)
{
    void *handle = astro_jit_so_all();
    if (handle) {
        return dlsym(handle, name);
    }
    else {
        return NULL;
    }
}

static node_dispatcher_func_t
astro_jit_search_so(node_hash_t h, const char *fname)
{
    // so name
    char so_name[MAX_PATH] = "";
    snprintf(so_name, MAX_PATH, "%lx.so", h);
    void *handle = astro_jit_dlopen(so_name);
    if (handle) {
        void *f = dlsym(handle, fname);
        _LOG("so_name:%s, f:%p", so_name, f);
        return f;
    }
    return NULL;
}

static node_dispatcher_func_t
astro_jit_search(node_hash_t h)
{
    char name[128];
    snprintf(name, 128, "SD_%lx", h);

    // search from all
    void *p = astro_jit_search_all(name);
    if (p) {
        _LOG("so_name:all.so");
        return p;
    }

    return astro_jit_search_so(h, name);
}

static bool
astro_jit_replace_dispatcher(NODE *n, node_hash_t h)
{
    node_dispatcher_func_t f = astro_jit_search(h);
    if (f) {
        _LOG("replace_dispatcher success h:%lx %p->%p", h, n->head.dispatcher, f);
        n->head.dispatcher = f;
        // n->head.dispatcher_name = ... // TODO: further optimization

        n->head.jit_status = JIT_STATUS_Compiled;
        return true;
    }
    else {
        LOG("replace_dispatcher failed h:%lx", h);
        return false;
    }
}

/* ---------------------------
 *  Protocol (type + size + payload)
 * --------------------------- */

enum astro_jit_message_type {
    REQ_QUERY   = 1,
    REQ_COMPILE = 2,
    REQ_CTL     = 9,
    RES_HIT     = 11,
    RES_MISS    = 12,
    RES_OBJ     = 13,
    RES_ERR     = 14,
};

static const char *
type_name(enum astro_jit_message_type type)
{
    switch (type) {
      case REQ_QUERY:   return "QUERY";
      case REQ_COMPILE: return "COMPILE";
      case REQ_CTL:     return "CTL";
      case RES_HIT:     return "RES:HIT";
      case RES_MISS:    return "RES:MISS";
      case RES_ERR:     return "RES:ERR";
      default:
        fprintf(stderr, "[BUG] unknown type:%d\n", type);
        exit(1);
    }
}

struct astro_jit_message {
    struct astro_jit_message_header {
        uint32_t type;
        uint32_t size;
        uint64_t hash;
    } h;
    char *payload; // malloc'ed (size+1), NUL-terminated for convenience
};

static int
write_all(int fd, const void *p, size_t n)
{
    const char *c = (const char *)p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            LOG("errno:%d", errno);
            return -1;
        }
        c += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int
read_all(int fd, void *p, size_t n)
{
    char *c = (char *)p;
    while (n) {
        ssize_t r = read(fd, c, n);

        if (r == 0) {
            LOG("read() => 0");
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG("errno:%d", errno);
            return -1;
        }
        c += (size_t)r;
        n -= (size_t)r;
    }
    LOG("read_all:finished");

    return 1;
}

static inline uint64_t htonll(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return __builtin_bswap64(x);
#else
  return x;
#endif
}

static inline uint64_t ntohll(uint64_t x)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  return __builtin_bswap64(x);
#else
  return x;
#endif
}

static int
send_msg(int fd, enum astro_jit_message_type type, node_hash_t hash, const void *payload, uint32_t size)
{
    struct astro_jit_message_header hdr = {
        .type = htonl(type),
        .size = htonl(size),
        .hash = htonll(hash),
    };
    if (write_all(fd, &hdr, sizeof(hdr)) != 0) {
        return -1;
    }
    if (size && write_all(fd, payload, size) != 0) {
        return -1;
    }
    return 0;
}

static int
recv_msg(int fd, struct astro_jit_message *m)
{
    struct astro_jit_message_header hdr;
    int rc = read_all(fd, &hdr, sizeof(hdr));
    if (rc <= 0) {
        LOG("rc:%d", rc);
        return rc; // 0: EOF, -1: error
    }
    m->h.type = ntohl(hdr.type);
    m->h.size = ntohl(hdr.size);
    m->h.hash = ntohll(hdr.hash);
    m->payload = NULL;

    LOG("type:%d size:%d hash:%lx", m->h.type, m->h.size, m->h.hash);

    if (hdr.size > 0) {
        m->payload = (char *)malloc((size_t)hdr.size + 1);
        if (!m->payload) return -1;
        rc = read_all(fd, m->payload, hdr.size);
        if (rc <= 0) {
            free(m->payload);
            m->payload = NULL;
            return rc;
        }
        m->payload[hdr.size] = '\0';
    }

    LOG("recv_msg: terminated");
    return 1;
}

/* ---------------------------
 *  L1 connect (Unix domain socket)
 * --------------------------- */

static int
connect_uds(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ---------------------------
 *  In-process request queue
 * --------------------------- */

enum astro_jit_ctl_type {
    CTL_CLEAR     = 0x01,
    CTL_SHUTDOWN  = 0x02,
    CTL_ONLY_L1   = 0x04,
};

struct astro_jit_request {
    enum astro_jit_message_type kind;
    NODE *node;         // patch target
    node_hash_t hash;
    enum astro_jit_ctl_type ctl;
    struct astro_jit_request *next;
};

struct astro_jit_request_queue {
    pthread_mutex_t lock;
    struct astro_jit_request *head;
    struct astro_jit_request *tail;
};

static void
astro_jit_request_queue_init(struct astro_jit_request_queue *q)
{
    pthread_mutex_init(&q->lock, NULL);
    q->head = q->tail = NULL;
}

static void
astro_jit_request_queue_push(struct astro_jit_request_queue *q, struct astro_jit_request *r)
{
    pthread_mutex_lock(&q->lock);
    {
        r->next = NULL;
        if (q->tail) q->tail->next = r;
        else q->head = r;
        q->tail = r;
    }
    pthread_mutex_unlock(&q->lock);
}

static struct astro_jit_request *
astro_jit_request_queue_pop_all(struct astro_jit_request_queue *q)
{
    struct astro_jit_request *h;

    pthread_mutex_lock(&q->lock);
    {
        h = q->head;
        q->head = q->tail = NULL;
    }
    pthread_mutex_unlock(&q->lock);

    return h;
}

static struct astro_jit_request *
astro_jit_request_alloc(enum astro_jit_message_type kind, NODE *n)
{
    struct astro_jit_request *r = (struct astro_jit_request *)calloc(1, sizeof(struct astro_jit_request));
    if (!r) {
        fprintf(stderr, "astro_jit_request_alloc: failed\n");
        exit(1);
    }
    r->kind = kind;
    r->node = n;
    r->hash = n ? HASH(n) : 0;
    return r;
}

static void
astro_jit_request_free(struct astro_jit_request *r)
{
    free(r);
}

/* ---------------------------
 *  L0 context + hooks
 * --------------------------- */

struct astro_jit {
    struct astro_jit_request_queue q;
    struct astro_jit_request_queue pq;
    int wake_pipe[2];                 // [0]=read, [1]=write
    pthread_t th;

    int l1_fd;
    const char *l1_uds_path;

    _Atomic(int) stop_flag;
};

static void
astro_jit_processing_set_add(struct astro_jit *aj, struct astro_jit_request *r)
{
    struct astro_jit_request_queue *q = &aj->pq;

    r->next = NULL;
    if (q->tail) {
        q->tail->next = r;
    }
    else {
        q->head = r;
    }
    q->tail = r;
}

static struct astro_jit_request *
astro_jit_processing_set_take(struct astro_jit *aj, node_hash_t hash)
{
    struct astro_jit_request *r = aj->pq.head, *prev = NULL;

    while (r) {
        if (r->hash == hash) {
            if (prev) {
                prev->next = r->next;
            }
            else {
                aj->pq.head = r->next;
            }
            if (aj->pq.tail == r) aj->pq.tail = NULL;
            return r;
        }
        prev = r;
        r = r->next;
    }
    return NULL;
}

static void
wake_l0(struct astro_jit *aj) {
    char b = 'x';
    if (write(aj->wake_pipe[1], &b, 1) <= 0) {
        fprintf(stderr, "[BUG] wake_l0: can not write to signal pipe\n");
        exit(1);
    }
    else {
        LOG("wkae_l0");
    }
}

static struct astro_jit *aj_get(void);

/* ---------------------------
 *  L0 -> L1 message senders
 * --------------------------- */

static int
l0_send_query_fd(int l1_fd, node_hash_t hash)
{
    LOG("L0->L1 h:%lx", hash);
    return send_msg(l1_fd, REQ_QUERY, hash, NULL, 0);
}

static int
l0_send_compile_fd(int l1_fd, node_hash_t hash, const char *src)
{
    size_t n = strlen(src);
    return send_msg(l1_fd, REQ_COMPILE, hash, src, n);
}

/* ---------------------------
 *  Thread main: event loop
 * --------------------------- */

int is_fd_readable_nb(int fd)
{
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
        .revents = 0,
    };

    int ret = poll(&pfd, 1, 0);  // timeout = 0 Å® non-blocking
    if (ret <= 0) {
        return 0;   // not readable (or error)
    }

    return (pfd.revents & POLLIN) != 0;
}

static bool
handle_l1_response(struct astro_jit *aj)
{
    // check readable or not
    if (!is_fd_readable_nb(aj->l1_fd)) {
        return true;
    }
    else {
        LOG("handle_l1_response");
    }

    struct astro_jit_message m;
    int rr = recv_msg(aj->l1_fd, &m);
    if (rr <= 0) {
        fprintf(stderr, "L0: L1 disconnected\n");
        return false;
    }

    LOG("L1->L0 type:%s h:%lx", type_name(m.h.type), m.h.hash);

    struct astro_jit_request *r = NULL;

    switch (m.h.type) {
      case RES_HIT:
        {
            r = astro_jit_processing_set_take(aj, m.h.hash);
            LOG("L1->L0 hit h:%lx req:%p", m.h.hash, r);
            if (r) {
                NODE *node = r->node;
                astro_jit_replace_dispatcher(node, m.h.hash);
            }
        }
        break;
      case RES_MISS:
        {
            r = astro_jit_processing_set_take(aj, m.h.hash);
            LOG("L1->L0 miss h:%lx req:%p", m.h.hash, r);

            if (r) {
                NODE *node = r->node;
                node->head.jit_status = JIT_STATUS_NotFound;
            }
            else {
                _LOG("MISS (h:%lx) but corresponding request is not found", m.h.hash, r);
            }
        }
        break;
      case RES_ERR:
        {
            r = astro_jit_processing_set_take(aj, m.h.hash);
            LOG("L1->L0 err h:%lx req:%p", m.h.hash, r);

            if (r) {
                NODE *node = r->node;
                node->head.jit_status = JIT_STATUS_NotFound;

                // payload: "hash\nmessage" (recommended)
                const char *p = m.payload ? m.payload : "";
                fprintf(stderr, "L0: L1 error: %s\n", p);
            }
            else {
                _LOG("ERR (h:%lx) but corresponding request is not found", m.h.hash, r);
            }
        }
        break;
      default:
        fprintf(stderr, "[BUG] L0: unexpected msg type=%u\n", m.h.type);
        exit(1);
    }

    if (r) astro_jit_request_free(r);
    free(m.payload);
    return true;
}

static void *
l0_main(void *arg)
{
    struct astro_jit *aj = (struct astro_jit *)arg;

    fprintf(stderr, "connect\n");
    aj->l1_fd = connect_uds(aj->l1_uds_path);

    if (aj->l1_fd < 0) {
        perror("L0: connect L1");
        exit(1);
        return NULL;
    }

    while (!atomic_load_explicit(&aj->stop_flag, memory_order_acquire)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(aj->wake_pipe[0], &rfds);
        FD_SET(aj->l1_fd, &rfds);
        int maxfd = (aj->wake_pipe[0] > aj->l1_fd) ? aj->wake_pipe[0] : aj->l1_fd;

        // wait for pipe
        LOG("sleep on select");
        int rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        LOG("wakeup on select");

        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("L0: select");
            break;
        }

        /* (A) Requests from interpreter thread */
        if (FD_ISSET(aj->wake_pipe[0], &rfds)) {
            {
                char buf[1024];
                if (read(aj->wake_pipe[0], buf, sizeof(buf)) < 0) {
                    perror("wake_pipe[0]");
                    exit(1);
                }
            }

            struct astro_jit_request *list = astro_jit_request_queue_pop_all(&aj->q);
            for (struct astro_jit_request *r = list; r;) {
                struct astro_jit_request *next = r->next;

                astro_jit_processing_set_add(aj, r);

                int src_send_rc = 0;

                switch (r->kind) {
                  case REQ_QUERY:
                    {
                        src_send_rc = l0_send_query_fd(aj->l1_fd, r->hash);
                    }
                    break;
                  case REQ_COMPILE:
                    {
                        const char *src = SPECIALIZED_SRC(r->node);
                        src_send_rc = l0_send_compile_fd(aj->l1_fd, r->hash, src);
                        LOG("l0_send_compile_fd sent - src:%s", src);
                        free((void *)src);
                    }
                    break;
                  default:
                    fprintf(stderr, "[BUG] unknown request:%d\n", r->kind);
                    exit(1);
                }

                if (src_send_rc != 0) {
                    fprintf(stderr, "L0: failed to send request to L1 (hash=%lx)\n", r->hash);
                }

                // check l1 message
                if (!handle_l1_response(aj)) goto terminate;

                r = next;
            }
        }

        /* (B) Replies from L1 */
        if (FD_ISSET(aj->l1_fd, &rfds)) {
            if (!handle_l1_response(aj)) goto terminate;
        }
    }

  terminate:
    close(aj->l1_fd);
    aj->l1_fd = -1;
    return NULL;
}

/* ---------------------------
 *  Start/stop helpers
 * --------------------------- */

struct astro_jit aj_body;

static struct astro_jit *
aj_get(void)
{
    return &aj_body;
}

int
astro_jit_start(const char *l1_uds_path)
{
    struct astro_jit *aj = aj_get();

    if (pipe(aj->wake_pipe) != 0) {
        perror("pipe");
        return -1;
    }

    astro_jit_request_queue_init(&aj->q);

    aj->l1_uds_path = l1_uds_path;
    atomic_store_explicit(&aj->stop_flag, 0, memory_order_release);

    if (pthread_create(&aj->th, NULL, l0_main, aj) != 0) {
        perror("pthread_create");
        exit(1);
    }
    return 0;
}

void
astro_jit_stop(void)
{
    struct astro_jit *aj = aj_get();

    atomic_store_explicit(&aj->stop_flag, 1, memory_order_release);
    wake_l0(aj);
    pthread_join(aj->th, NULL);
    close(aj->wake_pipe[0]);
    close(aj->wake_pipe[1]);
}

void
astro_jit_submit_query(NODE *node)
{
    if (node->head.jit_status != JIT_STATUS_Unknown) {
        LOG("n:%p h:%lx JITs:%d", node, HASH(node), node->head.jit_status);
        return;
    }
    else if (astro_jit_replace_dispatcher(node, HASH(node))) {
        LOG("n:%p h:%lx replaced", node, HASH(node));
        return;
    }
    else {
        LOG("n:%p h:%lx %d->%d", node, HASH(node), node->head.jit_status, JIT_STATUS_Querying);
        node->head.jit_status = JIT_STATUS_Querying;

        struct astro_jit *aj = aj_get();
        struct astro_jit_request *r = astro_jit_request_alloc(REQ_QUERY, node);
        if (r) {
            astro_jit_request_queue_push(&aj->q, r);
            wake_l0(aj);
        }
    }
}

void
astro_jit_submit_compile(NODE *node)
{
    if (node->head.jit_status == JIT_STATUS_Compiled) {
        LOG("%p already compiled", node);
        return;
    }
    else {
        LOG("h:%lx %d->%d", HASH(node), node->head.jit_status, JIT_STATUS_Compiling);
        struct astro_jit *aj = aj_get();
        struct astro_jit_request *r = astro_jit_request_alloc(REQ_COMPILE, node);
        if (r) {
            node->head.jit_status = JIT_STATUS_Compiling;
            astro_jit_request_queue_push(&aj->q, r);
            wake_l0(aj);
        }
    }
}

void
astro_jit_submit_ctl(enum astro_jit_ctl_type ctl)
{
    struct astro_jit *aj = aj_get();
    struct astro_jit_request *r = astro_jit_request_alloc(REQ_CTL, NULL);
    r->ctl = ctl;
    astro_jit_request_queue_push(&aj->q, r);
    wake_l0(aj);
}
