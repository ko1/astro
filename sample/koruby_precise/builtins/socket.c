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

/* Fill a sockaddr from (family, host, service).  `serv` is what getaddrinfo(3)
 * calls a service: a port number in decimal *or* a name from /etc/services
 * ("smtp").  AF_UNIX uses `host` as the path. */
static bool korb_sock_fill_addr_s(int family, const char *host, const char *serv,
                                  struct sockaddr_storage *ss, socklen_t *len) {
    memset(ss, 0, sizeof *ss);
    if (family == AF_UNIX) {
        struct sockaddr_un *un = (struct sockaddr_un *)ss;
        un->sun_family = AF_UNIX;
        snprintf(un->sun_path, sizeof un->sun_path, "%s", host ? host : "");
        *len = (socklen_t)sizeof(struct sockaddr_un);
        return true;
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    if (host == NULL || host[0] == '\0') hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo((host && host[0]) ? host : NULL, (serv && serv[0]) ? serv : NULL, &hints, &res) != 0 || res == NULL)
        return false;
    memcpy(ss, res->ai_addr, res->ai_addrlen);
    *len = res->ai_addrlen;
    freeaddrinfo(res);
    return true;
}

static bool korb_sock_fill_addr(int family, const char *host, int port,
                                struct sockaddr_storage *ss, socklen_t *len) {
    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", port);
    return korb_sock_fill_addr_s(family, host, portbuf, ss, len);
}

/* Render a port argument as a getaddrinfo service string: an Integer becomes
 * decimal, a String/Symbol passes through as a service name. */
static void korb_sock_serv_arg(CTX *c, VALUE v, char *buf, size_t cap) {
    if (FIXNUM_P(v))            snprintf(buf, cap, "%ld", (long)FIX2LONG(v));
    else if (KORB_STRING_P(v)) {
        uint32_t n; const char *const p = korb_str_cstr_len(v, &n);
        if (n >= cap) n = (uint32_t)cap - 1;
        memcpy(buf, p, n);
        buf[n] = '\0';
    }
    else if (SYMBOL_P(v))       snprintf(buf, cap, "%s", korb_sym_name(c->vm, SYM2ID(v)));
    else                        buf[0] = '\0';
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

/* connect(2) on a descriptor koruby made non-blocking returns EINPROGRESS and
 * completes asynchronously.  Park on POLLOUT and then read SO_ERROR — that is
 * the only way to learn whether it actually connected.  `host` is a stack
 * buffer, so it survives the park's GC. */
static RESULT korb_sock_finish_connect(CTX *c, VALUE *slots, int fd, const char *host) {
    struct pollfd pf; pf.fd = fd; pf.events = POLLOUT; pf.revents = 0;
    ssize_t ready = 0;
    const RESULT pr = korb_blop_poll_wait(c, slots, &pf, 1, -1.0, &ready);
    if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
    int err = 0; socklen_t el = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0)
        return korb_raise_errno(c, slots, errno, "connect", host);
    if (err != 0) return korb_raise_errno(c, slots, err, "connect", host);
    return RESULT_OK(LONG2FIX(0));
}

/* __sock_connect(fd, family, host, port_or_service) */
static RESULT korb_m_sock_connect(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int fam = korb_sock_family_of(c, VALUE_SLICE_GET(a, 1));
    char host[512], serv[64];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 2), host, sizeof host))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    korb_sock_serv_arg(c, VALUE_SLICE_GET(a, 3), serv, sizeof serv);
    struct sockaddr_storage ss; socklen_t len = 0;
    if (!korb_sock_fill_addr_s(fam, host, serv, &ss, &len))
        return korb_raise_errno(c, slots, ENOENT, "getaddrinfo", host);
    if (connect(fd, (struct sockaddr *)&ss, len) == 0) return RESULT_OK(LONG2FIX(0));
    if (errno == EINPROGRESS || errno == EALREADY || errno == EINTR)
        return korb_sock_finish_connect(c, slots, fd, host);
    return korb_raise_errno(c, slots, errno, "connect", host);
}

/* __sock_bind(fd, family, host, port) */
static RESULT korb_m_sock_bind(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int fam = korb_sock_family_of(c, VALUE_SLICE_GET(a, 1));
    char host[512];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 2), host, sizeof host))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    char serv[64];
    korb_sock_serv_arg(c, VALUE_SLICE_GET(a, 3), serv, sizeof serv);
    struct sockaddr_storage ss; socklen_t len = 0;
    if (!korb_sock_fill_addr_s(fam, host, serv, &ss, &len))
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
    korb_sword_t want = FIXNUM_P(VALUE_SLICE_GET(a, 1)) ? FIX2LONG(VALUE_SLICE_GET(a, 1)) : 4096;
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

/* __sock_recvfrom(fd, maxlen, flags) → [String, [family, port, host, addr]],
 * or nil when the call would block (the caller parks and retries).  MSG_DONTWAIT
 * is always added: koruby drives blocking through the scheduler, never by
 * stalling the native thread. */
static RESULT korb_m_sock_recvfrom(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    korb_sword_t want = FIXNUM_P(VALUE_SLICE_GET(a, 1)) ? FIX2LONG(VALUE_SLICE_GET(a, 1)) : 4096;
    if (want < 0) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "negative length");
    const int fl = (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0;
    char stackbuf[8192];
    char *const buf = (size_t)want <= sizeof stackbuf ? stackbuf : malloc((size_t)want ? (size_t)want : 1);
    if (!buf) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "out of memory");
    struct sockaddr_storage ss;
    socklen_t slen = sizeof ss;
    memset(&ss, 0, sizeof ss);
    const ssize_t r = recvfrom(fd, buf, (size_t)want, fl | MSG_DONTWAIT, (struct sockaddr *)&ss, &slen);
    if (r < 0) {
        const int e = errno;
        if (buf != stackbuf) free(buf);
        if (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) return RESULT_OK(KORB_NIL);
        return korb_raise_errno(c, slots, e, "recvfrom", "");
    }
    const RESULT sr = korb_str_new(c, slots, buf, (uint32_t)r);
    if (buf != stackbuf) free(buf);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    KORB_STR_ENC_SET(sr.value, KORB_ENC_BINARY);
    slots[0] = sr.value;                                  /* root the payload across the array builds */
    slots[1] = UNWRAP(korb_sock_addr_ary(c, slots + 1, (struct sockaddr *)&ss, slen ? slen : (socklen_t)sizeof ss));
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 2));
    VALUE_REF pair = VALUE_REF_AT(&slots[2]);
    CHECK(korb_ary_push_val(c, slots + 3, pair, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 3, pair, slots[1]));
    return RESULT_OK(VALUE_REF_GET(pair));
}

/* __sock_sendto(fd, string, flags, family, host, port) → byte count.
 * host nil → plain send(2) on a connected socket. */
static RESULT korb_m_sock_sendto(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const VALUE sv = VALUE_SLICE_GET(a, 1);
    if (!KORB_STRING_P(sv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const int fl = (VALUE_SLICE_LEN(a) >= 3 && FIXNUM_P(VALUE_SLICE_GET(a, 2))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 2)) : 0;
    struct sockaddr_storage ss;
    socklen_t alen = 0;
    if (VALUE_SLICE_LEN(a) >= 5 && VALUE_SLICE_GET(a, 4) != KORB_NIL) {
        const int fam = korb_sock_family_of(c, VALUE_SLICE_GET(a, 3));
        char host[512], serv[64];
        if (!korb_sock_cstr(VALUE_SLICE_GET(a, 4), host, sizeof host))
            return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
        korb_sock_serv_arg(c, VALUE_SLICE_LEN(a) >= 6 ? VALUE_SLICE_GET(a, 5) : KORB_NIL, serv, sizeof serv);
        if (!korb_sock_fill_addr_s(fam, host, serv, &ss, &alen))
            return korb_raise_errno(c, slots, ENOENT, "getaddrinfo", host);
    }
    uint32_t n; const char *const p = korb_str_cstr_len(sv, &n);   /* nothing allocates before the send */
    const ssize_t w = alen ? sendto(fd, p, n, fl, (struct sockaddr *)&ss, alen)
                           : send(fd, p, n, fl);
    if (w < 0) return korb_raise_errno(c, slots, errno, "send", "");
    return RESULT_OK(LONG2FIX((korb_sword_t)w));
}

/* __sock_hostent(name) → [canonical_name, [aliases], addrtype,
 *                          [packed address bytes...], [numeric strings...]].
 * Socket.gethostbyname wants the packed bytes, TCPSocket.gethostbyname the
 * dotted strings, so hand back both and let Ruby pick. */
static RESULT korb_m_sock_hostent(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    char host[512];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 0), host, sizeof host))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_CANONNAME;
    const int gr = getaddrinfo(host, NULL, &hints, &res);
    if (gr != 0) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "getaddrinfo: %s", gai_strerror(gr));
    const char *const canon = (res && res->ai_canonname) ? res->ai_canonname : host;
    const int fam = res ? res->ai_family : AF_INET;
    slots[0] = UNWRAP(korb_str_new(c, slots, canon, (uint32_t)strlen(canon)));
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 1));                 /* aliases: getaddrinfo exposes none */
    slots[2] = UNWRAP(korb_ary_new(c, slots + 2, 4));                 /* packed address bytes */
    slots[3] = UNWRAP(korb_ary_new(c, slots + 3, 4));                 /* numeric strings */
    VALUE_REF packed = VALUE_REF_AT(&slots[2]);
    VALUE_REF numeric = VALUE_REF_AT(&slots[3]);
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        char nb[NI_MAXHOST] = "";
        if (getnameinfo(ai->ai_addr, ai->ai_addrlen, nb, sizeof nb, NULL, 0, NI_NUMERICHOST) != 0) continue;
        const void *raw = NULL; uint32_t rawlen = 0;
        if (ai->ai_family == AF_INET)  { raw = &((struct sockaddr_in *)ai->ai_addr)->sin_addr;   rawlen = 4; }
        if (ai->ai_family == AF_INET6) { raw = &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr; rawlen = 16; }
        if (raw) {
            slots[4] = UNWRAP(korb_str_new(c, slots + 4, (const char *)raw, rawlen));
            KORB_STR_ENC_SET(slots[4], KORB_ENC_BINARY);
            CHECK(korb_ary_push_val(c, slots + 5, packed, slots[4]));
        }
        slots[4] = UNWRAP(korb_str_new(c, slots + 4, nb, (uint32_t)strlen(nb)));
        CHECK(korb_ary_push_val(c, slots + 5, numeric, slots[4]));
    }
    freeaddrinfo(res);
    slots[4] = UNWRAP(korb_ary_new(c, slots + 4, 5));
    VALUE_REF out = VALUE_REF_AT(&slots[4]);
    CHECK(korb_ary_push_val(c, slots + 5, out, slots[0]));
    CHECK(korb_ary_push_val(c, slots + 5, out, slots[1]));
    CHECK(korb_ary_push_val(c, slots + 5, out, LONG2FIX(fam)));
    CHECK(korb_ary_push_val(c, slots + 5, out, VALUE_REF_GET(packed)));
    CHECK(korb_ary_push_val(c, slots + 5, out, VALUE_REF_GET(numeric)));
    return RESULT_OK(VALUE_REF_GET(out));
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
    return RESULT_OK(LONG2FIX((korb_sword_t)w));
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

/* __sock_getnameinfo(packed_sockaddr, flags) → [hostname, service] */
static RESULT korb_m_sock_getnameinfo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE sv = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(sv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    uint32_t n; const char *const p = korb_str_cstr_len(sv, &n);
    struct sockaddr_storage ss;
    if (n < sizeof(sa_family_t) || n > sizeof ss)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "not a valid sockaddr (%u bytes)", n);
    memset(&ss, 0, sizeof ss);
    memcpy(&ss, p, n);                              /* copy out before anything allocates */
    const int flags = (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1))) ? (int)FIX2LONG(VALUE_SLICE_GET(a, 1)) : 0;
    char hbuf[NI_MAXHOST] = "", sbuf[NI_MAXSERV] = "";
    const int r = getnameinfo((struct sockaddr *)&ss, (socklen_t)n, hbuf, sizeof hbuf, sbuf, sizeof sbuf, flags);
    if (r != 0) return korb_raise(c, slots, KORB_E_RUNTIME, 0, "getnameinfo: %s", gai_strerror(r));
    slots[0] = UNWRAP(korb_ary_new(c, slots, 2));
    VALUE_REF ar = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, hbuf, (uint32_t)strlen(hbuf)));
    CHECK(korb_ary_push_val(c, slots + 2, ar, slots[1]));
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, sbuf, (uint32_t)strlen(sbuf)));
    CHECK(korb_ary_push_val(c, slots + 2, ar, slots[1]));
    return RESULT_OK(VALUE_REF_GET(ar));
}

/* __sock_pack(family_or_nil, port, host) → the packed sockaddr as a binary String.
 * Ruby code passes addresses around as the descriptive 4-element Array, but
 * Socket.sockaddr_in / Addrinfo#to_sockaddr are specified to hand back the raw
 * struct bytes (16 for sockaddr_in, 28 for sockaddr_in6, 110 for sockaddr_un),
 * and specs both check the size and feed the bytes back in.  nil family means
 * "infer from the host", which is what Socket.sockaddr_in(port, host) wants. */
static RESULT korb_m_sock_pack(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE fv = VALUE_SLICE_GET(a, 0);
    const int fam = (fv == KORB_NIL) ? AF_UNSPEC : korb_sock_family_of(c, fv);
    const VALUE pv = VALUE_SLICE_GET(a, 1);
    const int port = FIXNUM_P(pv) ? (int)FIX2LONG(pv) : 0;
    char host[512];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 2), host, sizeof host))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    struct sockaddr_storage ss; socklen_t len = 0;
    if (!korb_sock_fill_addr(fam, host, port, &ss, &len))
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "cannot resolve address '%s'", host);
    const RESULT sr = korb_str_new(c, slots, (const char *)&ss, (uint32_t)len);
    if (UNLIKELY(sr.state != KORB_NORMAL)) return sr;
    KORB_STR_ENC_SET(sr.value, KORB_ENC_BINARY);
    return sr;
}

/* __sock_unpack(str) → [family, port, host, addr], the same shape as
 * __sock_name / __sock_accept report. */
static RESULT korb_m_sock_unpack(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE sv = VALUE_SLICE_GET(a, 0);
    if (!KORB_STRING_P(sv)) return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    uint32_t n; const char *const p = korb_str_cstr_len(sv, &n);
    /* Copy out before anything can allocate: `p` points into a movable String. */
    struct sockaddr_storage ss;
    if (n < sizeof(sa_family_t) || n > sizeof ss)
        return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "not a valid sockaddr (%u bytes)", n);
    memset(&ss, 0, sizeof ss);
    memcpy(&ss, p, n);
    return korb_sock_addr_ary(c, slots, (struct sockaddr *)&ss, (socklen_t)n);
}

/* __sock_const("AF_INET") → the platform's numeric value.  A lookup keeps
 * lib/socket.rb free of platform ifdefs without inventing 40 constants whose
 * names Ruby could not spell. */
static RESULT korb_m_sock_const(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    char nm[64];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 0), nm, sizeof nm))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    static const struct { const char *n; korb_sword_t v; } cs[] = {
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
        {"INADDR_ANY", (korb_sword_t)INADDR_ANY}, {"INADDR_LOOPBACK", (korb_sword_t)INADDR_LOOPBACK},
        {"PF_UNSPEC", PF_UNSPEC},
#ifdef SOCK_RDM
        {"SOCK_RDM", SOCK_RDM},
#endif
#ifdef SOCK_PACKET
        {"SOCK_PACKET", SOCK_PACKET},
#endif
#ifdef AF_PACKET
        {"AF_PACKET", AF_PACKET}, {"PF_PACKET", PF_PACKET},
#endif
#ifdef IPPROTO_IPV6
        {"IPPROTO_IPV6", IPPROTO_IPV6},
#endif
#ifdef IPPROTO_ICMP
        {"IPPROTO_ICMP", IPPROTO_ICMP},
#endif
#ifdef IPPROTO_RAW
        {"IPPROTO_RAW", IPPROTO_RAW},
#endif
#ifdef IPPROTO_HOPOPTS
        {"IPPROTO_HOPOPTS", IPPROTO_HOPOPTS},
#endif
#ifdef IP_TTL
        {"IP_TTL", IP_TTL},
#endif
#ifdef IP_RECVTTL
        {"IP_RECVTTL", IP_RECVTTL},
#endif
#ifdef IP_PKTINFO
        {"IP_PKTINFO", IP_PKTINFO},
#endif
#ifdef IP_MTU
        {"IP_MTU", IP_MTU},
#endif
#ifdef IP_MULTICAST_TTL
        {"IP_MULTICAST_TTL", IP_MULTICAST_TTL}, {"IP_MULTICAST_LOOP", IP_MULTICAST_LOOP},
#endif
#ifdef IPV6_PKTINFO
        {"IPV6_PKTINFO", IPV6_PKTINFO},
#endif
#ifdef IPV6_NEXTHOP
        {"IPV6_NEXTHOP", IPV6_NEXTHOP},
#endif
#ifdef IPV6_V6ONLY
        {"IPV6_V6ONLY", IPV6_V6ONLY},
#endif
#ifdef TCP_CORK
        {"TCP_CORK", TCP_CORK},
#endif
#ifdef TCP_INFO
        {"TCP_INFO", TCP_INFO},
#endif
#ifdef TCP_KEEPIDLE
        {"TCP_KEEPIDLE", TCP_KEEPIDLE}, {"TCP_KEEPINTVL", TCP_KEEPINTVL}, {"TCP_KEEPCNT", TCP_KEEPCNT},
#endif
#ifdef UDP_CORK
        {"UDP_CORK", UDP_CORK},
#endif
#ifdef SCM_RIGHTS
        {"SCM_RIGHTS", SCM_RIGHTS},
#endif
#ifdef SCM_CREDENTIALS
        {"SCM_CREDENTIALS", SCM_CREDENTIALS},
#endif
#ifdef SCM_TIMESTAMP
        {"SCM_TIMESTAMP", SCM_TIMESTAMP},
#endif
#ifdef SO_REUSEPORT
        {"SO_REUSEPORT", SO_REUSEPORT},
#endif
#ifdef SO_DONTROUTE
        {"SO_DONTROUTE", SO_DONTROUTE},
#endif
#ifdef SO_OOBINLINE
        {"SO_OOBINLINE", SO_OOBINLINE},
#endif
#ifdef SO_RCVLOWAT
        {"SO_RCVLOWAT", SO_RCVLOWAT}, {"SO_SNDLOWAT", SO_SNDLOWAT},
#endif
#ifdef SO_RCVTIMEO
        {"SO_RCVTIMEO", SO_RCVTIMEO}, {"SO_SNDTIMEO", SO_SNDTIMEO},
#endif
#ifdef SO_ACCEPTCONN
        {"SO_ACCEPTCONN", SO_ACCEPTCONN},
#endif
#ifdef SO_PEERCRED
        {"SO_PEERCRED", SO_PEERCRED},
#endif
#ifdef MSG_TRUNC
        {"MSG_TRUNC", MSG_TRUNC}, {"MSG_CTRUNC", MSG_CTRUNC},
#endif
#ifdef MSG_MORE
        {"MSG_MORE", MSG_MORE},
#endif
#ifdef MSG_NOSIGNAL
        {"MSG_NOSIGNAL", MSG_NOSIGNAL},
#endif
#ifdef AI_ADDRCONFIG
        {"AI_ADDRCONFIG", AI_ADDRCONFIG}, {"AI_ALL", AI_ALL}, {"AI_V4MAPPED", AI_V4MAPPED},
#endif
#ifdef NI_NAMEREQD
        {"NI_NAMEREQD", NI_NAMEREQD}, {"NI_NOFQDN", NI_NOFQDN}, {"NI_DGRAM", NI_DGRAM},
#endif
#ifdef EAI_NONAME
        {"EAI_NONAME", EAI_NONAME}, {"EAI_AGAIN", EAI_AGAIN}, {"EAI_FAIL", EAI_FAIL},
        {"EAI_FAMILY", EAI_FAMILY}, {"EAI_SERVICE", EAI_SERVICE}, {"EAI_SOCKTYPE", EAI_SOCKTYPE},
#endif
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
    korb_class_def_cfn(c, obj, "__sock_pack",        korb_m_sock_pack,         3);
    korb_class_def_cfn(c, obj, "__sock_unpack",      korb_m_sock_unpack,       1);
    korb_class_def_cfn(c, obj, "__sock_getnameinfo", korb_m_sock_getnameinfo, -1);
    korb_class_def_cfn(c, obj, "__sock_recvfrom",    korb_m_sock_recvfrom,    -1);
    korb_class_def_cfn(c, obj, "__sock_sendto",      korb_m_sock_sendto,      -1);
    korb_class_def_cfn(c, obj, "__sock_hostent",     korb_m_sock_hostent,      1);
}
