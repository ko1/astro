/* koruby_precise — socket.c: the raw BSD-socket primitives.  The Ruby class
 * hierarchy (BasicSocket / IPSocket / TCPSocket / TCPServer / UNIXSocket /
 * UNIXServer / Socket / Addrinfo) lives in lib/socket.rb and drives these.
 * #included into korb_runtime.c's TU, after io.c (korb_io_make is used to wrap
 * an accepted descriptor).
 *
 * Addresses cross the boundary as plain Ruby data, never as packed sockaddr
 * blobs: an address is [family_string, port, hostname, numeric_address], the
 * same shape Addrinfo#to_a and BasicSocket#getsockname report.
 */
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* Build ["AF_INET"|"AF_INET6"|"AF_UNIX", port, host, addr] for a sockaddr. */
static RESULT korb_sock_addr_ary(CTX *c, VALUE *slots, const struct sockaddr *sa, socklen_t len) {
    char host[NI_MAXHOST] = "", serv[NI_MAXSERV] = "";
    const char *fam = "AF_UNSPEC";
    int port = 0;
    if (sa->sa_family == AF_INET || sa->sa_family == AF_INET6) {
        fam = sa->sa_family == AF_INET ? "AF_INET" : "AF_INET6";
        if (getnameinfo(sa, len, host, sizeof host, serv, sizeof serv,
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0)
            port = atoi(serv);
    } else if (sa->sa_family == AF_UNIX) {
        fam = "AF_UNIX";
        const struct sockaddr_un *un = (const struct sockaddr_un *)sa;
        snprintf(host, sizeof host, "%s", un->sun_path);
    }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF ar = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, fam, (uint32_t)strlen(fam)));
    CHECK(korb_ary_push_val(c, slots + 2, ar, slots[1]));
    CHECK(korb_ary_push_val(c, slots + 2, ar, LONG2FIX(port)));
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, host, (uint32_t)strlen(host)));
    CHECK(korb_ary_push_val(c, slots + 2, ar, slots[1]));
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, host, (uint32_t)strlen(host)));
    CHECK(korb_ary_push_val(c, slots + 2, ar, slots[1]));
    return RESULT_OK(VALUE_REF_GET(ar));
}

/* Fill a sockaddr from (family, host, port).  AF_UNIX uses `host` as the path. */
static bool korb_sock_fill_addr(int family, const char *host, int port,
                                struct sockaddr_storage *ss, socklen_t *len) {
    memset(ss, 0, sizeof *ss);
    if (family == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *)ss;
        un->sun_family = AF_UNIX;
        snprintf(un->sun_path, sizeof un->sun_path, "%s", host ? host : "");
        *len = (socklen_t)sizeof(struct sockaddr_un);
        return true;
    }
    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    if (host == NULL || host[0] == '\0') hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo((host && host[0]) ? host : NULL, portbuf, &hints, &res) != 0 || res == NULL) return false;
    memcpy(ss, res->ai_addr, res->ai_addrlen);
    *len = res->ai_addrlen;
    freeaddrinfo(res);
    return true;
}

static int korb_sock_family_of(CTX *c, VALUE v) {
    if (FIXNUM_P(v)) return (int)FIX2LONG(v);
    if (KORB_STRING_P(v)) {
        uint32_t n; const char *s = korb_str_cstr_len(v, &n);
        if (n >= 8 && !memcmp(s, "AF_INET6", 8)) return AF_INET6;
        if (n >= 7 && !memcmp(s, "AF_INET", 7))  return AF_INET;
        if (n >= 7 && !memcmp(s, "AF_UNIX", 7))  return AF_UNIX;
    }
    (void)c;
    return AF_INET;
}

/* Copy a String argument into a NUL-terminated stack buffer (nil → ""). */
static bool korb_sock_cstr(VALUE v, char *buf, size_t cap) {
    if (v == KORB_NIL) { buf[0] = '\0'; return true; }
    if (!KORB_STRING_P(v)) return false;
    uint32_t n; const char *s = korb_str_cstr_len(v, &n);
    if (n >= cap) n = (uint32_t)cap - 1;
    memcpy(buf, s, n); buf[n] = '\0';
    return true;
}

/* __sock_open(family, type, protocol) → fd */
static RESULT korb_m_sock_open(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fam = korb_sock_family_of(c, VALUE_SLICE_GET(a, 0));
    const int typ = FIXNUM_P(VALUE_SLICE_GET(a, 1)) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 1)) : SOCK_STREAM;
    const int pro = (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0;
    const int fd = socket(fam, typ, pro);
    if (fd < 0) return korb_raise_errno(c, slots, errno, "socket", "");
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    return RESULT_OK(LONG2FIX(fd));
}

/* __sock_connect(fd, family, host, port) */
static RESULT korb_m_sock_connect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int fam = korb_sock_family_of(c, VALUE_SLICE_GET(a, 1));
    char host[512];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 2), host, sizeof host))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const int port = FIXNUM_P(VALUE_SLICE_GET(a, 3)) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 3)) : 0;
    struct sockaddr_storage ss; socklen_t len = 0;
    if (!korb_sock_fill_addr(fam, host, port, &ss, &len))
        return korb_raise_errno(c, slots, ENOENT, "getaddrinfo", host);
    if (connect(fd, (struct sockaddr *)&ss, len) != 0) return korb_raise_errno(c, slots, errno, "connect", host);
    return RESULT_OK(LONG2FIX(0));
}

/* __sock_bind(fd, family, host, port) */
static RESULT korb_m_sock_bind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int fam = korb_sock_family_of(c, VALUE_SLICE_GET(a, 1));
    char host[512];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 2), host, sizeof host))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const int port = FIXNUM_P(VALUE_SLICE_GET(a, 3)) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 3)) : 0;
    struct sockaddr_storage ss; socklen_t len = 0;
    if (!korb_sock_fill_addr(fam, host, port, &ss, &len))
        return korb_raise_errno(c, slots, ENOENT, "getaddrinfo", host);
    if (bind(fd, (struct sockaddr *)&ss, len) != 0) return korb_raise_errno(c, slots, errno, "bind", host);
    return RESULT_OK(LONG2FIX(0));
}

static RESULT korb_m_sock_listen(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int bl = FIXNUM_P(VALUE_SLICE_GET(a, 1)) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 1)) : 5;
    if (listen(fd, bl) != 0) return korb_raise_errno(c, slots, errno, "listen", "");
    /* "poll says readable" does not guarantee accept(2) won't block — a spurious
       wakeup or a connection aborted between poll and accept both do.  Make the
       listening fd non-blocking so accept returns EAGAIN instead of freezing the
       scheduler; lib/socket.rb parks on POLL and retries. */
    const int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return RESULT_OK(LONG2FIX(0));
}

/* __sock_accept(fd) → [fd, [family, port, host, addr]] */
static RESULT korb_m_sock_accept(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    struct sockaddr_storage ss; socklen_t len = sizeof ss;
    const int nfd = accept(fd, (struct sockaddr *)&ss, &len);
    if (nfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return RESULT_OK(KORB_NIL);
        return korb_raise_errno(c, slots, errno, "accept", "");
    }
    (void)fcntl(nfd, F_SETFD, FD_CLOEXEC);
    slots[0] = UNWRAP(korb_sock_addr_ary(c, slots, (struct sockaddr *)&ss, len));
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
    VALUE_REF pair = VALUE_REF_AT(&slots[1]);
    CHECK(korb_ary_push_val(c, slots + 2, pair, LONG2FIX(nfd)));
    CHECK(korb_ary_push_val(c, slots + 2, pair, slots[0]));
    return RESULT_OK(VALUE_REF_GET(pair));
}

/* __sock_name(fd, peer?) → [family, port, host, addr] */
static RESULT korb_m_sock_name(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const bool peer = KORB_TRUTHY(VALUE_SLICE_GET(a, 1));
    struct sockaddr_storage ss; socklen_t len = sizeof ss;
    const int r = peer ? getpeername(fd, (struct sockaddr *)&ss, &len)
                       : getsockname(fd, (struct sockaddr *)&ss, &len);
    if (r != 0) return korb_raise_errno(c, slots, errno, peer ? "getpeername" : "getsockname", "");
    return korb_sock_addr_ary(c, slots, (struct sockaddr *)&ss, len);
}

/* __sock_getaddrinfo(host, port, family, socktype) → [[family, port, host, addr, socktype, protocol], …] */
static RESULT korb_m_sock_getaddrinfo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    char host[512], portbuf[64];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 0), host, sizeof host))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const VALUE pv = VALUE_SLICE_GET(a, 1);
    if (FIXNUM_P(pv)) snprintf(portbuf, sizeof portbuf, "%ld", (long)FIX2LONG(pv));
    else if (!korb_sock_cstr(pv, portbuf, sizeof portbuf)) portbuf[0] = '\0';
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = (VALUE_SLICE_LEN(a) >= 3 && VALUE_SLICE_GET(a, 2) != KORB_NIL)
                        ? korb_sock_family_of(c, VALUE_SLICE_GET(a, 2)) : AF_UNSPEC;
    hints.ai_socktype = (VALUE_SLICE_LEN(a) >= 4 && FIXNUM_P(VALUE_SLICE_GET(a, 3)))
                        ? (int)FIX2LONG(VALUE_SLICE_GET(a, 3)) : 0;
    if (host[0] == '\0') hints.ai_flags = AI_PASSIVE;
    const int gr = getaddrinfo(host[0] ? host : NULL, portbuf[0] ? portbuf : NULL, &hints, &res);
    if (gr != 0) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "getaddrinfo: %s", gai_strerror(gr));
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF list = VALUE_REF_AT(&slots[0]);
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        slots[1] = UNWRAP(korb_sock_addr_ary(c, slots + 1, ai->ai_addr, ai->ai_addrlen));
        VALUE_REF one = VALUE_REF_AT(&slots[1]);
        CHECK(korb_ary_push_val(c, slots + 2, one, LONG2FIX(ai->ai_socktype)));
        CHECK(korb_ary_push_val(c, slots + 2, one, LONG2FIX(ai->ai_protocol)));
        CHECK(korb_ary_push_val(c, slots + 2, list, VALUE_REF_GET(one)));
    }
    freeaddrinfo(res);
    return RESULT_OK(VALUE_REF_GET(list));
}

/* __sock_setopt(fd, level, name, int_value) / __sock_getopt(fd, level, name) */
static RESULT korb_m_sock_setopt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int lv = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
    const int nm = (int)FIX2LONG(VALUE_SLICE_GET(a, 2));
    const VALUE v = VALUE_SLICE_GET(a, 3);
    int iv = KORB_TRUTHY(v) ? 1 : 0;
    if (FIXNUM_P(v)) iv = (int)FIX2LONG(v);
    if (setsockopt(fd, lv, nm, &iv, sizeof iv) != 0) return korb_raise_errno(c, slots, errno, "setsockopt", "");
    return RESULT_OK(LONG2FIX(0));
}
static RESULT korb_m_sock_getopt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int lv = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
    const int nm = (int)FIX2LONG(VALUE_SLICE_GET(a, 2));
    int iv = 0; socklen_t l = sizeof iv;
    if (getsockopt(fd, lv, nm, &iv, &l) != 0) return korb_raise_errno(c, slots, errno, "getsockopt", "");
    return RESULT_OK(LONG2FIX(iv));
}

static RESULT korb_m_sock_shutdown(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int how = (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 1)) : SHUT_RDWR;
    if (shutdown(fd, how) != 0) return korb_raise_errno(c, slots, errno, "shutdown", "");
    return RESULT_OK(LONG2FIX(0));
}

/* __sock_recv(fd, maxlen, flags) → String ("" at EOF) */
static RESULT korb_m_sock_recv(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    intptr_t want = FIXNUM_P(VALUE_SLICE_GET(a, 1)) ? FIX2LONG(VALUE_SLICE_GET(a, 1)) : 4096;
    if (want < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative length");
    const int fl = (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0;
    char stackbuf[8192];
    char *buf = (size_t)want <= sizeof stackbuf ? stackbuf : malloc((size_t)want);
    if (!buf) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory");
    const ssize_t r = recv(fd, buf, (size_t)want, fl | MSG_DONTWAIT);
    if (r < 0) {
        const int e = errno;
        if (buf != stackbuf) free(buf);
        if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) return RESULT_OK(KORB_NIL);   /* caller parks + retries */
        return korb_raise_errno(c, slots, e, "recv", "");
    }
    const RESULT sr = korb_str_new(c, slots, buf, (uint32_t)r);
    if (buf != stackbuf) free(buf);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    KORB_STR_ENC_SET(sr.value, KORB_ENC_BINARY);
    return sr;
}

/* __sock_send(fd, string, flags) → byte count */
static RESULT korb_m_sock_send(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const VALUE sv = VALUE_SLICE_GET(a, 1);
    if (!KORB_STRING_P(sv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const int fl = (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0;
    uint32_t n; const char *p = korb_str_cstr_len(sv, &n);
    const ssize_t w = send(fd, p, n, fl);
    if (w < 0) return korb_raise_errno(c, slots, errno, "send", "");
    return RESULT_OK(LONG2FIX((intptr_t)w));
}

/* __sock_pair(family, type) → [fd, fd] */
static RESULT korb_m_sock_pair(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fam = (VALUE_SLICE_LEN(a) >= 1) ? korb_sock_family_of(c, VALUE_SLICE_GET(a, 0)) : AF_UNIX;
    const int typ = (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 1)) : SOCK_STREAM;
    int fds[2];
    if (socketpair(fam, typ, 0, fds) != 0) return korb_raise_errno(c, slots, errno, "socketpair", "");
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    slots[0] = UNWRAP(korb_ary_new(c, slots, 2));
    VALUE_REF ar = VALUE_REF_AT(&slots[0]);
    CHECK(korb_ary_push_val(c, slots + 1, ar, LONG2FIX(fds[0])));
    CHECK(korb_ary_push_val(c, slots + 1, ar, LONG2FIX(fds[1])));
    return RESULT_OK(VALUE_REF_GET(ar));
}

static RESULT korb_m_sock_hostname(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    char h[256] = "";
    if (gethostname(h, sizeof h) != 0) return korb_raise_errno(c, slots, errno, "gethostname", "");
    h[sizeof h - 1] = '\0';
    return korb_str_new(c, slots, h, (uint32_t)strlen(h));
}

/* __sock_const("AF_INET") → the platform's numeric value.  A lookup keeps
 * lib/socket.rb free of platform ifdefs without inventing 40 constants whose
 * names Ruby could not spell. */
static RESULT korb_m_sock_const(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    char nm[64];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 0), nm, sizeof nm))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    static const struct { const char *n; intptr_t v; } cs[] = {
        {"AF_INET", AF_INET}, {"AF_INET6", AF_INET6}, {"AF_UNIX", AF_UNIX},
        {"AF_UNSPEC", AF_UNSPEC}, {"PF_INET", PF_INET}, {"PF_INET6", PF_INET6},
        {"PF_UNIX", PF_UNIX}, {"SOCK_STREAM", SOCK_STREAM}, {"SOCK_DGRAM", SOCK_DGRAM},
        {"SOCK_RAW", SOCK_RAW}, {"SOCK_SEQPACKET", SOCK_SEQPACKET},
        {"SOL_SOCKET", SOL_SOCKET}, {"IPPROTO_TCP", IPPROTO_TCP}, {"IPPROTO_UDP", IPPROTO_UDP},
        {"IPPROTO_IP", IPPROTO_IP}, {"SO_REUSEADDR", SO_REUSEADDR}, {"SO_KEEPALIVE", SO_KEEPALIVE},
        {"SO_BROADCAST", SO_BROADCAST}, {"SO_LINGER", SO_LINGER}, {"SO_SNDBUF", SO_SNDBUF},
        {"SO_RCVBUF", SO_RCVBUF}, {"SO_TYPE", SO_TYPE}, {"SO_ERROR", SO_ERROR},
        {"TCP_NODELAY", TCP_NODELAY}, {"SHUT_RD", SHUT_RD}, {"SHUT_WR", SHUT_WR},
        {"SHUT_RDWR", SHUT_RDWR}, {"MSG_PEEK", MSG_PEEK}, {"MSG_OOB", MSG_OOB},
        {"MSG_DONTROUTE", MSG_DONTROUTE}, {"MSG_WAITALL", MSG_WAITALL},
        {"MSG_DONTWAIT", MSG_DONTWAIT},
        {"AI_PASSIVE", AI_PASSIVE}, {"AI_CANONNAME", AI_CANONNAME}, {"AI_NUMERICHOST", AI_NUMERICHOST},
        {"NI_NUMERICHOST", NI_NUMERICHOST}, {"NI_NUMERICSERV", NI_NUMERICSERV},
        {"INADDR_ANY", (intptr_t)INADDR_ANY}, {"INADDR_LOOPBACK", (intptr_t)INADDR_LOOPBACK},
    };
    for (size_t i = 0; i < sizeof cs / sizeof cs[0]; i++)
        if (!strcmp(nm, cs[i].n)) return RESULT_OK(LONG2FIX(cs[i].v));
    return RESULT_OK(KORB_NIL);
}

void korb_init_socket(CTX *c, VALUE *slots) {
    (void)slots;
    const VALUE obj = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
    korb_class_def_cfn(c, obj, "__sock_open",        korb_m_sock_open,        -1);
    korb_class_def_cfn(c, obj, "__sock_connect",     korb_m_sock_connect,      4);
    korb_class_def_cfn(c, obj, "__sock_bind",        korb_m_sock_bind,         4);
    korb_class_def_cfn(c, obj, "__sock_listen",      korb_m_sock_listen,       2);
    korb_class_def_cfn(c, obj, "__sock_accept",      korb_m_sock_accept,       1);
    korb_class_def_cfn(c, obj, "__sock_name",        korb_m_sock_name,         2);
    korb_class_def_cfn(c, obj, "__sock_getaddrinfo", korb_m_sock_getaddrinfo, -1);
    korb_class_def_cfn(c, obj, "__sock_setopt",      korb_m_sock_setopt,       4);
    korb_class_def_cfn(c, obj, "__sock_getopt",      korb_m_sock_getopt,       3);
    korb_class_def_cfn(c, obj, "__sock_shutdown",    korb_m_sock_shutdown,    -1);
    korb_class_def_cfn(c, obj, "__sock_recv",        korb_m_sock_recv,        -1);
    korb_class_def_cfn(c, obj, "__sock_send",        korb_m_sock_send,        -1);
    korb_class_def_cfn(c, obj, "__sock_pair",        korb_m_sock_pair,        -1);
    korb_class_def_cfn(c, obj, "__sock_hostname",    korb_m_sock_hostname,     0);
    korb_class_def_cfn(c, obj, "__sock_const",       korb_m_sock_const,        1);
}
