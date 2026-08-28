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
#include <ifaddrs.h>
#include <netinet/udp.h>
/* UDP_CORK / UDP_GRO live in linux/udp.h, which redefines struct udphdr — take
 * just the numbers rather than the header. */
#if defined(__linux__) && !defined(UDP_CORK)
#  define UDP_CORK 1
#endif
#if defined(__linux__) && !defined(UDP_ENCAP)
#  define UDP_ENCAP 100
#endif

/* Build ["AF_INET"|"AF_INET6"|"AF_UNIX", port, host, addr] for a sockaddr. */
static RESULT korb_sock_addr_ary(CTX *c, VALUE *slots, const struct sockaddr *sa, socklen_t len) {
    char host[NI_MAXHOST] = "", serv[NI_MAXSERV] = "";
    const char *fam = "AF_UNSPEC";
    int port = 0;
    if (sa->sa_family == AF_INET || sa->sa_family == AF_INET6) {
        fam = sa->sa_family == AF_INET ? "AF_INET" : "AF_INET6";
        /* a caller-supplied sockaddr String may be shorter than the struct
         * (Socket.sockaddr_in packs 28 bytes for IPv6); getnameinfo wants the
         * family's own length */
        const socklen_t want = (sa->sa_family == AF_INET) ? (socklen_t)sizeof(struct sockaddr_in)
                                                          : (socklen_t)sizeof(struct sockaddr_in6);
        if (getnameinfo(sa, want, host, sizeof host, serv, sizeof serv,
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0)
            port = atoi(serv);
        (void)len;
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
    if (FIXNUM_P(v))            snprintf(buf, cap, "%lld", (long long)FIX2LONG(v));
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
/* IO::TimeoutError — a Ruby-side class, so raise a plain exception carrying it. */
static RESULT
korb_raise_io_timeout(CTX *c, VALUE *slots, const char *msg)
{
    const VALUE io = korb_const_get(c->vm, korb_intern(c->vm, "IO", 2));
    VALUE cls = KORB_NIL;
    if (KORB_CLASS_P(io)) {
        const uint32_t ix = korb_const_index_owned(c->vm, korb_intern(c->vm, "TimeoutError", 12), io);
        if (ix != UINT32_MAX) cls = c->vm->const_vals[ix];
    }
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "%s", msg);
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value))
        ARO_STORE(c, VAL2EXC(r.value), (VALUE *)(uintptr_t)&VAL2EXC(r.value)->exc_class, slots[0]);
    return r;
}

static RESULT korb_sock_finish_connect(CTX *c, VALUE *slots, int fd, const char *host, double timeout) {
    struct pollfd pf; pf.fd = fd; pf.events = POLLOUT; pf.revents = 0;
    ssize_t ready = 0;
    const RESULT pr = korb_blop_poll_wait(c, slots, &pf, 1, timeout, &ready);
    if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
    if (timeout >= 0.0 && ready == 0)                  /* the wait expired before the connect finished */
        return korb_raise_io_timeout(c, slots, "Connect timed out!");
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
    double tmo = -1.0;                                 /* optional 5th arg: seconds, or nil for no limit */
    const VALUE tv = (VALUE_SLICE_LEN(a) >= 5) ? VALUE_SLICE_GET(a, 4) : KORB_NIL;
    if (tv != KORB_NIL && !korb_num_to_d(tv, &tmo)) tmo = -1.0;
    /* A bounded connect has to be able to give up, so it goes out non-blocking
     * and the wait below enforces the deadline; the flag is put back after. */
    int saved_fl = -1;
    if (tmo >= 0.0) {
        saved_fl = fcntl(fd, F_GETFL);
        if (saved_fl >= 0 && !(saved_fl & O_NONBLOCK)) (void)fcntl(fd, F_SETFL, saved_fl | O_NONBLOCK);
        else saved_fl = -1;
    }
    const int cr = connect(fd, (struct sockaddr *)&ss, len);
    const int cerr = errno;
    if (cr == 0) {
        if (saved_fl >= 0) (void)fcntl(fd, F_SETFL, saved_fl);
        return RESULT_OK(LONG2FIX(0));
    }
    if (cerr == EINPROGRESS || cerr == EALREADY || cerr == EINTR) {
        const RESULT r = korb_sock_finish_connect(c, slots, fd, host, tmo);
        if (saved_fl >= 0) (void)fcntl(fd, F_SETFL, saved_fl);
        return r;
    }
    if (saved_fl >= 0) (void)fcntl(fd, F_SETFL, saved_fl);
    return korb_raise_errno(c, slots, cerr, "connect", host);
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

/* Socket::ResolutionError (Ruby 3.3+, a SocketError) — the class lives in
 * lib/socket.rb, so raise a plain exception carrying it as #class. */
static RESULT
korb_raise_resolution_error_code(CTX *c, VALUE *slots, const char *msg, int code)
{
    const VALUE sock = korb_const_get(c->vm, korb_intern(c->vm, "Socket", 6));
    VALUE cls = KORB_NIL;
    if (KORB_CLASS_P(sock)) {
        const uint32_t nm = korb_intern(c->vm, "ResolutionError", 15);
        const uint32_t ix = korb_const_index_owned(c->vm, nm, sock);
        if (ix != UINT32_MAX) cls = c->vm->const_vals[ix];
    }
    slots[0] = KORB_CLASS_P(cls) ? cls : KORB_NIL;
    RESULT r = korb_raise(c, slots + 1, KORB_E_RUNTIME, 0, "%s", msg);
    if (KORB_CLASS_P(slots[0]) && KORB_EXC_P(r.value)) {
        slots[1] = r.value;
        ARO_STORE(c, VAL2EXC(slots[1]), (VALUE *)(uintptr_t)&VAL2EXC(slots[1])->exc_class, slots[0]);
        /* #error_code is the EAI_* value getaddrinfo(3) returned */
        korb_exc_ivar_set(c, slots + 2, VALUE_REF_AT(&slots[1]),
                          ID2SYM(korb_intern(c->vm, "@error_code", 11)), LONG2FIX(code));
        r.value = slots[1];
    }
    return r;
}
static RESULT korb_raise_resolution_error(CTX *c, VALUE *slots, const char *msg) {
    return korb_raise_resolution_error_code(c, slots, msg, 0);
}

/* __sock_getaddrinfo(host, port, family, socktype[, flags[, protocol]]) → [[family, port, host, addr, socktype, protocol], …] */
static RESULT korb_m_sock_getaddrinfo(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    char host[512], portbuf[64];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 0), host, sizeof host))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const VALUE pv = VALUE_SLICE_GET(a, 1);
    if (FIXNUM_P(pv)) snprintf(portbuf, sizeof portbuf, "%lld", (long long)FIX2LONG(pv));
    else if (!korb_sock_cstr(pv, portbuf, sizeof portbuf)) portbuf[0] = '\0';
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = (VALUE_SLICE_LEN(a) >= 3 && VALUE_SLICE_GET(a, 2) != KORB_NIL)
                        ? korb_sock_family_of(c, VALUE_SLICE_GET(a, 2)) : AF_UNSPEC;
    hints.ai_socktype = (VALUE_SLICE_LEN(a) >= 4 && FIXNUM_P(VALUE_SLICE_GET(a, 3)))
                        ? (int)FIX2LONG(VALUE_SLICE_GET(a, 3)) : 0;
    hints.ai_protocol = (VALUE_SLICE_LEN(a) >= 6 && FIXNUM_P(VALUE_SLICE_GET(a, 5)))
                        ? (int)FIX2LONG(VALUE_SLICE_GET(a, 5)) : 0;
    /* the caller's ai_flags decide; with no host and no AI_PASSIVE, getaddrinfo(3)
     * answers the loopback (a client address), which is what CRuby reports */
    if (VALUE_SLICE_LEN(a) >= 5 && FIXNUM_P(VALUE_SLICE_GET(a, 4))) hints.ai_flags = (int)FIX2LONG(VALUE_SLICE_GET(a, 4));
    const int gr = getaddrinfo(host[0] ? host : NULL, portbuf[0] ? portbuf : NULL, &hints, &res);
    if (gr != 0) { char m[192]; snprintf(m, sizeof m, "getaddrinfo: %s", gai_strerror(gr)); return korb_raise_resolution_error_code(c, slots, m, gr); }
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF list = VALUE_REF_AT(&slots[0]);
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        slots[1] = UNWRAP(korb_sock_addr_ary(c, slots + 1, ai->ai_addr, ai->ai_addrlen));
        VALUE_REF one = VALUE_REF_AT(&slots[1]);
        CHECK(korb_ary_push_val(c, slots + 2, one, LONG2FIX(ai->ai_family)));   /* CRuby's 5th element */
        CHECK(korb_ary_push_val(c, slots + 2, one, LONG2FIX(ai->ai_socktype)));
        CHECK(korb_ary_push_val(c, slots + 2, one, LONG2FIX(ai->ai_protocol)));
        CHECK(korb_ary_push_val(c, slots + 2, list, VALUE_REF_GET(one)));
    }
    freeaddrinfo(res);
    return RESULT_OK(VALUE_REF_GET(list));
}

/* __sock_servbyname(name, proto) → port (host byte order), or nil. */
static RESULT korb_m_sock_servbyname(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    char nm[128], pr[32];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 0), nm, sizeof nm))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    if (VALUE_SLICE_LEN(a) < 2 || !korb_sock_cstr(VALUE_SLICE_GET(a, 1), pr, sizeof pr)) snprintf(pr, sizeof pr, "tcp");
    const struct servent *const se = getservbyname(nm, pr);
    if (se == NULL) return RESULT_OK(KORB_NIL);
    return RESULT_OK(LONG2FIX((korb_sword_t)ntohs((uint16_t)se->s_port)));
}

/* __sock_servbyport(port, proto) → the service name, or nil. */
static RESULT korb_m_sock_servbyport(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE pv = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!FIXNUM_P(pv)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into Integer");
    char pr[32];
    if (VALUE_SLICE_LEN(a) < 2 || !korb_sock_cstr(VALUE_SLICE_GET(a, 1), pr, sizeof pr)) snprintf(pr, sizeof pr, "tcp");
    const struct servent *const se = getservbyport(htons((uint16_t)FIX2LONG(pv)), pr);
    if (se == NULL) return RESULT_OK(KORB_NIL);
    return korb_str_new(c, slots, se->s_name, (uint32_t)strlen(se->s_name));
}

/* __sock_hostbyaddr(packed_addr, family) → [name, [aliases], addrtype, packed]. */
static RESULT korb_m_sock_hostbyaddr(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const VALUE av = VALUE_SLICE_GET(a, 0);
    if (UNLIKELY(!KORB_STRING_P(av)))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    const uint32_t alen = VAL2STR(av)->len;
    int fam = (VALUE_SLICE_LEN(a) >= 2 && FIXNUM_P(VALUE_SLICE_GET(a, 1)))
                ? (int)FIX2LONG(VALUE_SLICE_GET(a, 1)) : (alen == 16 ? AF_INET6 : AF_INET);
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof ss);
    socklen_t sl;
    if (fam == AF_INET6) {
        if (UNLIKELY(alen != 16)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid address length");
        struct sockaddr_in6 *const s6 = (struct sockaddr_in6 *)&ss;
        s6->sin6_family = AF_INET6;
        memcpy(&s6->sin6_addr, korb_strbuf_data(VAL2STR(av)->buf), 16);
        sl = sizeof *s6;
    } else {
        if (UNLIKELY(alen != 4)) return korb_raise(c, slots, KORB_E_ARGUMENT, 0, "invalid address length");
        struct sockaddr_in *const s4 = (struct sockaddr_in *)&ss;
        s4->sin_family = AF_INET;
        memcpy(&s4->sin_addr, korb_strbuf_data(VAL2STR(av)->buf), 4);
        sl = sizeof *s4;
        fam = AF_INET;
    }
    char hbuf[NI_MAXHOST];
    if (getnameinfo((const struct sockaddr *)&ss, sl, hbuf, sizeof hbuf, NULL, 0, NI_NAMEREQD) != 0)
        return korb_raise_resolution_error(c, slots, "gethostbyaddr: address not found");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 4));
    VALUE_REF out = VALUE_REF_AT(&slots[0]);
    slots[1] = UNWRAP(korb_str_new(c, slots + 1, hbuf, (uint32_t)strlen(hbuf)));
    CHECK(korb_ary_push_val(c, slots + 2, out, slots[1]));
    slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 0));                    /* aliases: getnameinfo has none */
    CHECK(korb_ary_push_val(c, slots + 2, out, slots[1]));
    CHECK(korb_ary_push_val(c, slots + 2, out, LONG2FIX(fam)));
    CHECK(korb_ary_push_val(c, slots + 2, out, VALUE_SLICE_GET(a, 0)));
    return RESULT_OK(VALUE_REF_GET(out));
}

/* __sock_getopt_raw(fd, level, name) → the option's bytes as a String. */
static RESULT korb_m_sock_getopt_raw(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int lv = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
    const int nm = (int)FIX2LONG(VALUE_SLICE_GET(a, 2));
    char buf[256];
    socklen_t len = sizeof buf;
    if (getsockopt(fd, lv, nm, buf, &len) != 0) return korb_raise_errno(c, slots, errno, "getsockopt", "");
    return korb_str_new(c, slots, buf, (uint32_t)len);
}

/* __sock_send_fd(fd, payload_fd) — hand a descriptor to the peer over a UNIX
 * socket (SCM_RIGHTS).  One byte of ordinary data goes with it, as CRuby does,
 * because a control message needs a carrier. */
static RESULT korb_m_sock_send_fd(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int payload = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
    char dummy = 0;
    struct iovec iov; iov.iov_base = &dummy; iov.iov_len = 1;
    union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } cm;
    memset(&cm, 0, sizeof cm);
    struct msghdr msg; memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = cm.buf; msg.msg_controllen = sizeof cm.buf;
    struct cmsghdr *const h = CMSG_FIRSTHDR(&msg);
    h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &payload, sizeof(int));
    for (;;) {
        const ssize_t n = sendmsg(fd, &msg, 0);
        if (n >= 0) return RESULT_OK(LONG2FIX(n));
        if (errno == EINTR) continue;
        return korb_raise_errno(c, slots, errno, "sendmsg", "");
    }
}

/* __sock_recv_fd(fd) → the descriptor the peer passed, or nil if none came. */
static RESULT korb_m_sock_recv_fd(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    char dummy = 0;
    struct iovec iov; iov.iov_base = &dummy; iov.iov_len = 1;
    union { struct cmsghdr align; char buf[CMSG_SPACE(sizeof(int))]; } cm;
    struct msghdr msg;
    for (;;) {
        memset(&cm, 0, sizeof cm);
        memset(&msg, 0, sizeof msg);
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = cm.buf; msg.msg_controllen = sizeof cm.buf;
        const ssize_t n = recvmsg(fd, &msg, 0);
        if (n >= 0) break;
        if (errno == EINTR) continue;
        if (korb_io_would_block(errno)) {               /* park like the other reads do */
            struct pollfd pf; pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
            ssize_t ready = 0;
            const RESULT pr = korb_blop_poll_wait(c, slots, &pf, 1, -1.0, &ready);
            if (UNLIKELY(pr.state != KORB_NORMAL)) return pr;
            continue;
        }
        return korb_raise_errno(c, slots, errno, "recvmsg", "");
    }
    for (struct cmsghdr *h = CMSG_FIRSTHDR(&msg); h != NULL; h = CMSG_NXTHDR(&msg, h))
        if (h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS) {
            int got; memcpy(&got, CMSG_DATA(h), sizeof got);
            return RESULT_OK(LONG2FIX(got));
        }
    return RESULT_OK(KORB_NIL);
}

/* __sock_ifaddrs() → [[family, "addr"], …] for every interface with an IP. */
static RESULT korb_m_sock_ifaddrs(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    struct ifaddrs *head = NULL;
    if (getifaddrs(&head) != 0) return korb_raise_errno(c, slots, errno, "getifaddrs", "");
    slots[0] = UNWRAP(korb_ary_new(c, slots, 8));
    VALUE_REF list = VALUE_REF_AT(&slots[0]);
    RESULT r = RESULT_OK(KORB_NIL);
    for (const struct ifaddrs *p = head; p != NULL && r.state == KORB_NORMAL; p = p->ifa_next) {
        if (p->ifa_addr == NULL) continue;
        const int fam = p->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6) continue;
        char host[NI_MAXHOST];
        const socklen_t sl = (fam == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
        if (getnameinfo(p->ifa_addr, sl, host, sizeof host, NULL, 0, NI_NUMERICHOST) != 0) continue;
        char *const pct = strchr(host, '%');            /* drop a scope id: Addrinfo takes the bare address */
        if (pct) *pct = '\0';
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
        VALUE_REF one = VALUE_REF_AT(&slots[1]);
        CHECK(korb_ary_push_val(c, slots + 2, one, LONG2FIX(fam)));
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, host, (uint32_t)strlen(host)));
        CHECK(korb_ary_push_val(c, slots + 3, one, slots[2]));
        r = korb_ary_push_val(c, slots + 2, list, VALUE_REF_GET(one));
    }
    freeifaddrs(head);
    if (UNLIKELY(r.state != KORB_NORMAL)) return r;
    return RESULT_OK(VALUE_REF_GET(list));
}

/* __sock_setopt(fd, level, name, int_value) / __sock_getopt(fd, level, name) */
static RESULT korb_m_sock_setopt(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    const int fd = (int)FIX2LONG(VALUE_SLICE_GET(a, 0));
    const int lv = (int)FIX2LONG(VALUE_SLICE_GET(a, 1));
    const int nm = (int)FIX2LONG(VALUE_SLICE_GET(a, 2));
    const VALUE v = VALUE_SLICE_GET(a, 3);
    if (KORB_STRING_P(v)) {   /* a packed struct (SO_LINGER, ip_mreq, …) goes through verbatim */
        const KorbString *const sv = VAL2STR(v);
        if (setsockopt(fd, lv, nm, korb_strbuf_data(sv->buf), (socklen_t)sv->len) != 0)
            return korb_raise_errno(c, slots, errno, "setsockopt", "");
        return RESULT_OK(LONG2FIX(0));
    }
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
    if (gr != 0) { char m[192]; snprintf(m, sizeof m, "getaddrinfo: %s", gai_strerror(gr)); return korb_raise_resolution_error_code(c, slots, m, gr); }
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
    int port = FIXNUM_P(pv) ? (int)FIX2LONG(pv) : 0;
    if (!FIXNUM_P(pv) && KORB_STRING_P(pv)) {          /* a service name ("smtp") or a numeric String */
        char sv[64];
        if (korb_sock_cstr(pv, sv, sizeof sv)) {
            char *endp = NULL;
            const long n2 = strtol(sv, &endp, 10);
            if (endp && *endp == '\0' && endp != sv) port = (int)n2;
            else {
                const struct servent *const se = getservbyname(sv, NULL);
                if (se) port = ntohs((uint16_t)se->s_port);
            }
        }
    }
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
/* Every constant the platform actually defines; the #ifdef guards keep this
 * portable without a configure step. */
#define KSC(x) { #x, (korb_sword_t)(x) }
static const struct korb_sock_const { const char *n; korb_sword_t v; } korb_sock_consts[] = {
#ifdef AF_ALG
        KSC(AF_ALG),
#endif
#ifdef AF_APPLETALK
        KSC(AF_APPLETALK),
#endif
#ifdef AF_AX25
        KSC(AF_AX25),
#endif
#ifdef AF_BLUETOOTH
        KSC(AF_BLUETOOTH),
#endif
#ifdef AF_CAN
        KSC(AF_CAN),
#endif
#ifdef AF_DECnet
        KSC(AF_DECnet),
#endif
#ifdef AF_IB
        KSC(AF_IB),
#endif
#ifdef AF_IPX
        KSC(AF_IPX),
#endif
#ifdef AF_ISDN
        KSC(AF_ISDN),
#endif
#ifdef AF_KCM
        KSC(AF_KCM),
#endif
#ifdef AF_KEY
        KSC(AF_KEY),
#endif
#ifdef AF_LLC
        KSC(AF_LLC),
#endif
#ifdef AF_LOCAL
        KSC(AF_LOCAL),
#endif
#ifdef AF_MAX
        KSC(AF_MAX),
#endif
#ifdef AF_MPLS
        KSC(AF_MPLS),
#endif
#ifdef AF_NETLINK
        KSC(AF_NETLINK),
#endif
#ifdef AF_PPPOX
        KSC(AF_PPPOX),
#endif
#ifdef AF_RDS
        KSC(AF_RDS),
#endif
#ifdef AF_ROUTE
        KSC(AF_ROUTE),
#endif
#ifdef AF_SNA
        KSC(AF_SNA),
#endif
#ifdef AF_TIPC
        KSC(AF_TIPC),
#endif
#ifdef AF_VSOCK
        KSC(AF_VSOCK),
#endif
#ifdef AF_XDP
        KSC(AF_XDP),
#endif
#ifdef AI_NUMERICSERV
        KSC(AI_NUMERICSERV),
#endif
#ifdef EAI_ADDRFAMILY
        KSC(EAI_ADDRFAMILY),
#endif
#ifdef EAI_BADFLAGS
        KSC(EAI_BADFLAGS),
#endif
#ifdef EAI_MEMORY
        KSC(EAI_MEMORY),
#endif
#ifdef EAI_NODATA
        KSC(EAI_NODATA),
#endif
#ifdef EAI_OVERFLOW
        KSC(EAI_OVERFLOW),
#endif
#ifdef EAI_SYSTEM
        KSC(EAI_SYSTEM),
#endif
#ifdef IFF_ALLMULTI
        KSC(IFF_ALLMULTI),
#endif
#ifdef IFF_AUTOMEDIA
        KSC(IFF_AUTOMEDIA),
#endif
#ifdef IFF_BROADCAST
        KSC(IFF_BROADCAST),
#endif
#ifdef IFF_DEBUG
        KSC(IFF_DEBUG),
#endif
#ifdef IFF_DYNAMIC
        KSC(IFF_DYNAMIC),
#endif
#ifdef IFF_LOOPBACK
        KSC(IFF_LOOPBACK),
#endif
#ifdef IFF_MASTER
        KSC(IFF_MASTER),
#endif
#ifdef IFF_MULTICAST
        KSC(IFF_MULTICAST),
#endif
#ifdef IFF_NOARP
        KSC(IFF_NOARP),
#endif
#ifdef IFF_NOTRAILERS
        KSC(IFF_NOTRAILERS),
#endif
#ifdef IFF_POINTOPOINT
        KSC(IFF_POINTOPOINT),
#endif
#ifdef IFF_PORTSEL
        KSC(IFF_PORTSEL),
#endif
#ifdef IFF_PROMISC
        KSC(IFF_PROMISC),
#endif
#ifdef IFF_RUNNING
        KSC(IFF_RUNNING),
#endif
#ifdef IFF_SLAVE
        KSC(IFF_SLAVE),
#endif
#ifdef IFF_UP
        KSC(IFF_UP),
#endif
#ifdef IFNAMSIZ
        KSC(IFNAMSIZ),
#endif
#ifdef IF_NAMESIZE
        KSC(IF_NAMESIZE),
#endif
#ifdef INADDR_ALLHOSTS_GROUP
        KSC(INADDR_ALLHOSTS_GROUP),
#endif
#ifdef INADDR_BROADCAST
        KSC(INADDR_BROADCAST),
#endif
#ifdef INADDR_MAX_LOCAL_GROUP
        KSC(INADDR_MAX_LOCAL_GROUP),
#endif
#ifdef INADDR_NONE
        KSC(INADDR_NONE),
#endif
#ifdef INADDR_UNSPEC_GROUP
        KSC(INADDR_UNSPEC_GROUP),
#endif
#ifdef INET6_ADDRSTRLEN
        KSC(INET6_ADDRSTRLEN),
#endif
#ifdef INET_ADDRSTRLEN
        KSC(INET_ADDRSTRLEN),
#endif
#ifdef IPPORT_RESERVED
        KSC(IPPORT_RESERVED),
#endif
#ifdef IPPORT_USERRESERVED
        KSC(IPPORT_USERRESERVED),
#endif
#ifdef IPPROTO_AH
        KSC(IPPROTO_AH),
#endif
#ifdef IPPROTO_DSTOPTS
        KSC(IPPROTO_DSTOPTS),
#endif
#ifdef IPPROTO_EGP
        KSC(IPPROTO_EGP),
#endif
#ifdef IPPROTO_ESP
        KSC(IPPROTO_ESP),
#endif
#ifdef IPPROTO_FRAGMENT
        KSC(IPPROTO_FRAGMENT),
#endif
#ifdef IPPROTO_ICMPV6
        KSC(IPPROTO_ICMPV6),
#endif
#ifdef IPPROTO_IDP
        KSC(IPPROTO_IDP),
#endif
#ifdef IPPROTO_IGMP
        KSC(IPPROTO_IGMP),
#endif
#ifdef IPPROTO_NONE
        KSC(IPPROTO_NONE),
#endif
#ifdef IPPROTO_PUP
        KSC(IPPROTO_PUP),
#endif
#ifdef IPPROTO_ROUTING
        KSC(IPPROTO_ROUTING),
#endif
#ifdef IPPROTO_TP
        KSC(IPPROTO_TP),
#endif
#ifdef IPV6_CHECKSUM
        KSC(IPV6_CHECKSUM),
#endif
#ifdef IPV6_DONTFRAG
        KSC(IPV6_DONTFRAG),
#endif
#ifdef IPV6_DSTOPTS
        KSC(IPV6_DSTOPTS),
#endif
#ifdef IPV6_HOPLIMIT
        KSC(IPV6_HOPLIMIT),
#endif
#ifdef IPV6_HOPOPTS
        KSC(IPV6_HOPOPTS),
#endif
#ifdef IPV6_JOIN_GROUP
        KSC(IPV6_JOIN_GROUP),
#endif
#ifdef IPV6_LEAVE_GROUP
        KSC(IPV6_LEAVE_GROUP),
#endif
#ifdef IPV6_MTU_DISCOVER
        KSC(IPV6_MTU_DISCOVER),
#endif
#ifdef IPV6_MULTICAST_HOPS
        KSC(IPV6_MULTICAST_HOPS),
#endif
#ifdef IPV6_MULTICAST_IF
        KSC(IPV6_MULTICAST_IF),
#endif
#ifdef IPV6_MULTICAST_LOOP
        KSC(IPV6_MULTICAST_LOOP),
#endif
#ifdef IPV6_PATHMTU
        KSC(IPV6_PATHMTU),
#endif
#ifdef IPV6_RECVDSTOPTS
        KSC(IPV6_RECVDSTOPTS),
#endif
#ifdef IPV6_RECVERR
        KSC(IPV6_RECVERR),
#endif
#ifdef IPV6_RECVHOPLIMIT
        KSC(IPV6_RECVHOPLIMIT),
#endif
#ifdef IPV6_RECVHOPOPTS
        KSC(IPV6_RECVHOPOPTS),
#endif
#ifdef IPV6_RECVPATHMTU
        KSC(IPV6_RECVPATHMTU),
#endif
#ifdef IPV6_RECVPKTINFO
        KSC(IPV6_RECVPKTINFO),
#endif
#ifdef IPV6_RECVRTHDR
        KSC(IPV6_RECVRTHDR),
#endif
#ifdef IPV6_RECVTCLASS
        KSC(IPV6_RECVTCLASS),
#endif
#ifdef IPV6_RTHDR
        KSC(IPV6_RTHDR),
#endif
#ifdef IPV6_RTHDRDSTOPTS
        KSC(IPV6_RTHDRDSTOPTS),
#endif
#ifdef IPV6_RTHDR_TYPE_0
        KSC(IPV6_RTHDR_TYPE_0),
#endif
#ifdef IPV6_TCLASS
        KSC(IPV6_TCLASS),
#endif
#ifdef IPV6_UNICAST_HOPS
        KSC(IPV6_UNICAST_HOPS),
#endif
#ifdef IP_ADD_MEMBERSHIP
        KSC(IP_ADD_MEMBERSHIP),
#endif
#ifdef IP_ADD_SOURCE_MEMBERSHIP
        KSC(IP_ADD_SOURCE_MEMBERSHIP),
#endif
#ifdef IP_BLOCK_SOURCE
        KSC(IP_BLOCK_SOURCE),
#endif
#ifdef IP_DEFAULT_MULTICAST_LOOP
        KSC(IP_DEFAULT_MULTICAST_LOOP),
#endif
#ifdef IP_DEFAULT_MULTICAST_TTL
        KSC(IP_DEFAULT_MULTICAST_TTL),
#endif
#ifdef IP_DROP_MEMBERSHIP
        KSC(IP_DROP_MEMBERSHIP),
#endif
#ifdef IP_DROP_SOURCE_MEMBERSHIP
        KSC(IP_DROP_SOURCE_MEMBERSHIP),
#endif
#ifdef IP_FREEBIND
        KSC(IP_FREEBIND),
#endif
#ifdef IP_HDRINCL
        KSC(IP_HDRINCL),
#endif
#ifdef IP_IPSEC_POLICY
        KSC(IP_IPSEC_POLICY),
#endif
#ifdef IP_MAX_MEMBERSHIPS
        KSC(IP_MAX_MEMBERSHIPS),
#endif
#ifdef IP_MINTTL
        KSC(IP_MINTTL),
#endif
#ifdef IP_MSFILTER
        KSC(IP_MSFILTER),
#endif
#ifdef IP_MTU_DISCOVER
        KSC(IP_MTU_DISCOVER),
#endif
#ifdef IP_MULTICAST_IF
        KSC(IP_MULTICAST_IF),
#endif
#ifdef IP_OPTIONS
        KSC(IP_OPTIONS),
#endif
#ifdef IP_PASSSEC
        KSC(IP_PASSSEC),
#endif
#ifdef IP_PKTOPTIONS
        KSC(IP_PKTOPTIONS),
#endif
#ifdef IP_PMTUDISC_DO
        KSC(IP_PMTUDISC_DO),
#endif
#ifdef IP_PMTUDISC_DONT
        KSC(IP_PMTUDISC_DONT),
#endif
#ifdef IP_PMTUDISC_WANT
        KSC(IP_PMTUDISC_WANT),
#endif
#ifdef IP_RECVERR
        KSC(IP_RECVERR),
#endif
#ifdef IP_RECVOPTS
        KSC(IP_RECVOPTS),
#endif
#ifdef IP_RECVRETOPTS
        KSC(IP_RECVRETOPTS),
#endif
#ifdef IP_RECVTOS
        KSC(IP_RECVTOS),
#endif
#ifdef IP_RETOPTS
        KSC(IP_RETOPTS),
#endif
#ifdef IP_ROUTER_ALERT
        KSC(IP_ROUTER_ALERT),
#endif
#ifdef IP_TOS
        KSC(IP_TOS),
#endif
#ifdef IP_TRANSPARENT
        KSC(IP_TRANSPARENT),
#endif
#ifdef IP_UNBLOCK_SOURCE
        KSC(IP_UNBLOCK_SOURCE),
#endif
#ifdef IP_XFRM_POLICY
        KSC(IP_XFRM_POLICY),
#endif
#ifdef MCAST_BLOCK_SOURCE
        KSC(MCAST_BLOCK_SOURCE),
#endif
#ifdef MCAST_EXCLUDE
        KSC(MCAST_EXCLUDE),
#endif
#ifdef MCAST_INCLUDE
        KSC(MCAST_INCLUDE),
#endif
#ifdef MCAST_JOIN_GROUP
        KSC(MCAST_JOIN_GROUP),
#endif
#ifdef MCAST_JOIN_SOURCE_GROUP
        KSC(MCAST_JOIN_SOURCE_GROUP),
#endif
#ifdef MCAST_LEAVE_GROUP
        KSC(MCAST_LEAVE_GROUP),
#endif
#ifdef MCAST_LEAVE_SOURCE_GROUP
        KSC(MCAST_LEAVE_SOURCE_GROUP),
#endif
#ifdef MCAST_MSFILTER
        KSC(MCAST_MSFILTER),
#endif
#ifdef MCAST_UNBLOCK_SOURCE
        KSC(MCAST_UNBLOCK_SOURCE),
#endif
#ifdef MSG_CONFIRM
        KSC(MSG_CONFIRM),
#endif
#ifdef MSG_EOR
        KSC(MSG_EOR),
#endif
#ifdef MSG_ERRQUEUE
        KSC(MSG_ERRQUEUE),
#endif
#ifdef MSG_FASTOPEN
        KSC(MSG_FASTOPEN),
#endif
#ifdef MSG_FIN
        KSC(MSG_FIN),
#endif
#ifdef MSG_PROXY
        KSC(MSG_PROXY),
#endif
#ifdef MSG_RST
        KSC(MSG_RST),
#endif
#ifdef MSG_SYN
        KSC(MSG_SYN),
#endif
#ifdef NI_MAXHOST
        KSC(NI_MAXHOST),
#endif
#ifdef NI_MAXSERV
        KSC(NI_MAXSERV),
#endif
#ifdef PF_ALG
        KSC(PF_ALG),
#endif
#ifdef PF_APPLETALK
        KSC(PF_APPLETALK),
#endif
#ifdef PF_AX25
        KSC(PF_AX25),
#endif
#ifdef PF_BLUETOOTH
        KSC(PF_BLUETOOTH),
#endif
#ifdef PF_CAN
        KSC(PF_CAN),
#endif
#ifdef PF_DECnet
        KSC(PF_DECnet),
#endif
#ifdef PF_IB
        KSC(PF_IB),
#endif
#ifdef PF_IPX
        KSC(PF_IPX),
#endif
#ifdef PF_ISDN
        KSC(PF_ISDN),
#endif
#ifdef PF_KCM
        KSC(PF_KCM),
#endif
#ifdef PF_KEY
        KSC(PF_KEY),
#endif
#ifdef PF_LLC
        KSC(PF_LLC),
#endif
#ifdef PF_LOCAL
        KSC(PF_LOCAL),
#endif
#ifdef PF_MAX
        KSC(PF_MAX),
#endif
#ifdef PF_MPLS
        KSC(PF_MPLS),
#endif
#ifdef PF_NETLINK
        KSC(PF_NETLINK),
#endif
#ifdef PF_PPPOX
        KSC(PF_PPPOX),
#endif
#ifdef PF_RDS
        KSC(PF_RDS),
#endif
#ifdef PF_ROUTE
        KSC(PF_ROUTE),
#endif
#ifdef PF_SNA
        KSC(PF_SNA),
#endif
#ifdef PF_TIPC
        KSC(PF_TIPC),
#endif
#ifdef PF_VSOCK
        KSC(PF_VSOCK),
#endif
#ifdef PF_XDP
        KSC(PF_XDP),
#endif
#ifdef SCM_TIMESTAMPING
        KSC(SCM_TIMESTAMPING),
#endif
#ifdef SCM_TIMESTAMPNS
        KSC(SCM_TIMESTAMPNS),
#endif
#ifdef SCM_WIFI_STATUS
        KSC(SCM_WIFI_STATUS),
#endif
#ifdef SOCK_CLOEXEC
        KSC(SOCK_CLOEXEC),
#endif
#ifdef SOCK_NONBLOCK
        KSC(SOCK_NONBLOCK),
#endif
#ifdef SOL_IP
        KSC(SOL_IP),
#endif
#ifdef SOL_TCP
        KSC(SOL_TCP),
#endif
#ifdef SOL_UDP
        KSC(SOL_UDP),
#endif
#ifdef SOMAXCONN
        KSC(SOMAXCONN),
#endif
#ifdef SO_ATTACH_FILTER
        KSC(SO_ATTACH_FILTER),
#endif
#ifdef SO_BINDTODEVICE
        KSC(SO_BINDTODEVICE),
#endif
#ifdef SO_BPF_EXTENSIONS
        KSC(SO_BPF_EXTENSIONS),
#endif
#ifdef SO_BUSY_POLL
        KSC(SO_BUSY_POLL),
#endif
#ifdef SO_DEBUG
        KSC(SO_DEBUG),
#endif
#ifdef SO_DETACH_FILTER
        KSC(SO_DETACH_FILTER),
#endif
#ifdef SO_DOMAIN
        KSC(SO_DOMAIN),
#endif
#ifdef SO_GET_FILTER
        KSC(SO_GET_FILTER),
#endif
#ifdef SO_INCOMING_CPU
        KSC(SO_INCOMING_CPU),
#endif
#ifdef SO_INCOMING_NAPI_ID
        KSC(SO_INCOMING_NAPI_ID),
#endif
#ifdef SO_LOCK_FILTER
        KSC(SO_LOCK_FILTER),
#endif
#ifdef SO_MARK
        KSC(SO_MARK),
#endif
#ifdef SO_MAX_PACING_RATE
        KSC(SO_MAX_PACING_RATE),
#endif
#ifdef SO_NOFCS
        KSC(SO_NOFCS),
#endif
#ifdef SO_NO_CHECK
        KSC(SO_NO_CHECK),
#endif
#ifdef SO_PASSCRED
        KSC(SO_PASSCRED),
#endif
#ifdef SO_PASSSEC
        KSC(SO_PASSSEC),
#endif
#ifdef SO_PEEK_OFF
        KSC(SO_PEEK_OFF),
#endif
#ifdef SO_PEERNAME
        KSC(SO_PEERNAME),
#endif
#ifdef SO_PEERSEC
        KSC(SO_PEERSEC),
#endif
#ifdef SO_PRIORITY
        KSC(SO_PRIORITY),
#endif
#ifdef SO_PROTOCOL
        KSC(SO_PROTOCOL),
#endif
#ifdef SO_RCVBUFFORCE
        KSC(SO_RCVBUFFORCE),
#endif
#ifdef SO_RXQ_OVFL
        KSC(SO_RXQ_OVFL),
#endif
#ifdef SO_SECURITY_AUTHENTICATION
        KSC(SO_SECURITY_AUTHENTICATION),
#endif
#ifdef SO_SECURITY_ENCRYPTION_NETWORK
        KSC(SO_SECURITY_ENCRYPTION_NETWORK),
#endif
#ifdef SO_SECURITY_ENCRYPTION_TRANSPORT
        KSC(SO_SECURITY_ENCRYPTION_TRANSPORT),
#endif
#ifdef SO_SELECT_ERR_QUEUE
        KSC(SO_SELECT_ERR_QUEUE),
#endif
#ifdef SO_SNDBUFFORCE
        KSC(SO_SNDBUFFORCE),
#endif
#ifdef SO_TIMESTAMP
        KSC(SO_TIMESTAMP),
#endif
#ifdef SO_TIMESTAMPING
        KSC(SO_TIMESTAMPING),
#endif
#ifdef SO_TIMESTAMPNS
        KSC(SO_TIMESTAMPNS),
#endif
#ifdef SO_WIFI_STATUS
        KSC(SO_WIFI_STATUS),
#endif
#ifdef TCP_CONGESTION
        KSC(TCP_CONGESTION),
#endif
#ifdef TCP_COOKIE_TRANSACTIONS
        KSC(TCP_COOKIE_TRANSACTIONS),
#endif
#ifdef TCP_DEFER_ACCEPT
        KSC(TCP_DEFER_ACCEPT),
#endif
#ifdef TCP_FASTOPEN
        KSC(TCP_FASTOPEN),
#endif
#ifdef TCP_LINGER2
        KSC(TCP_LINGER2),
#endif
#ifdef TCP_MAXSEG
        KSC(TCP_MAXSEG),
#endif
#ifdef TCP_MD5SIG
        KSC(TCP_MD5SIG),
#endif
#ifdef TCP_QUEUE_SEQ
        KSC(TCP_QUEUE_SEQ),
#endif
#ifdef TCP_QUICKACK
        KSC(TCP_QUICKACK),
#endif
#ifdef TCP_REPAIR
        KSC(TCP_REPAIR),
#endif
#ifdef TCP_REPAIR_OPTIONS
        KSC(TCP_REPAIR_OPTIONS),
#endif
#ifdef TCP_REPAIR_QUEUE
        KSC(TCP_REPAIR_QUEUE),
#endif
#ifdef TCP_SYNCNT
        KSC(TCP_SYNCNT),
#endif
#ifdef TCP_THIN_DUPACK
        KSC(TCP_THIN_DUPACK),
#endif
#ifdef TCP_THIN_LINEAR_TIMEOUTS
        KSC(TCP_THIN_LINEAR_TIMEOUTS),
#endif
#ifdef TCP_TIMESTAMP
        KSC(TCP_TIMESTAMP),
#endif
#ifdef TCP_USER_TIMEOUT
        KSC(TCP_USER_TIMEOUT),
#endif
#ifdef TCP_WINDOW_CLAMP
        KSC(TCP_WINDOW_CLAMP),
#endif
#ifdef UDP_CORK
        KSC(UDP_CORK),
#endif

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
#undef KSC
#define KORB_SOCK_NCONST (sizeof korb_sock_consts / sizeof korb_sock_consts[0])

/* __sock_const(name) → the value, or nil when this platform lacks it. */
static RESULT korb_m_sock_const(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self;
    char nm[64];
    if (!korb_sock_cstr(VALUE_SLICE_GET(a, 0), nm, sizeof nm))
        return korb_raise(c, slots, KORB_E_TYPE, 0, "no implicit conversion into String");
    for (size_t i = 0; i < KORB_SOCK_NCONST; i++)
        if (!strcmp(nm, korb_sock_consts[i].n)) return RESULT_OK(LONG2FIX(korb_sock_consts[i].v));
    return RESULT_OK(KORB_NIL);
}

/* __sock_const_list() → [[name, value], …] for every constant this build has,
 * so lib/socket.rb can define them all without listing each name twice. */
static RESULT korb_m_sock_const_list(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    slots[0] = UNWRAP(korb_ary_new(c, slots, (uint32_t)KORB_SOCK_NCONST));
    VALUE_REF list = VALUE_REF_AT(&slots[0]);
    for (size_t i = 0; i < KORB_SOCK_NCONST; i++) {
        slots[1] = UNWRAP(korb_ary_new(c, slots + 1, 2));
        VALUE_REF one = VALUE_REF_AT(&slots[1]);
        slots[2] = UNWRAP(korb_str_new(c, slots + 2, korb_sock_consts[i].n, (uint32_t)strlen(korb_sock_consts[i].n)));
        CHECK(korb_ary_push_val(c, slots + 3, one, slots[2]));
        CHECK(korb_ary_push_val(c, slots + 3, one, LONG2FIX(korb_sock_consts[i].v)));
        CHECK(korb_ary_push_val(c, slots + 2, list, VALUE_REF_GET(one)));
    }
    return RESULT_OK(VALUE_REF_GET(list));
}

void korb_init_socket(CTX *c, VALUE *slots) {
    (void)slots;
    const VALUE obj = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
    korb_class_def_cfn(c, obj, "__sock_open",        korb_m_sock_open,        -1);
    korb_class_def_cfn(c, obj, "__sock_connect",     korb_m_sock_connect,     -1);
    korb_class_def_cfn(c, obj, "__sock_bind",        korb_m_sock_bind,         4);
    korb_class_def_cfn(c, obj, "__sock_listen",      korb_m_sock_listen,       2);
    korb_class_def_cfn(c, obj, "__sock_accept",      korb_m_sock_accept,       1);
    korb_class_def_cfn(c, obj, "__sock_servbyname",  korb_m_sock_servbyname,  -1);
    korb_class_def_cfn(c, obj, "__sock_hostbyaddr",  korb_m_sock_hostbyaddr,  -1);
    korb_class_def_cfn(c, obj, "__sock_servbyport",  korb_m_sock_servbyport,  -1);
    korb_class_def_cfn(c, obj, "__sock_ifaddrs",     korb_m_sock_ifaddrs,      0);
    korb_class_def_cfn(c, obj, "__sock_send_fd",     korb_m_sock_send_fd,      2);
    korb_class_def_cfn(c, obj, "__sock_recv_fd",     korb_m_sock_recv_fd,      1);
    korb_class_def_cfn(c, obj, "__sock_getopt_raw",  korb_m_sock_getopt_raw,   3);
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
    korb_class_def_cfn(c, obj, "__sock_const_list",  korb_m_sock_const_list,   0);
    korb_class_def_cfn(c, obj, "__sock_pack",        korb_m_sock_pack,         3);
    korb_class_def_cfn(c, obj, "__sock_unpack",      korb_m_sock_unpack,       1);
    korb_class_def_cfn(c, obj, "__sock_getnameinfo", korb_m_sock_getnameinfo, -1);
    korb_class_def_cfn(c, obj, "__sock_recvfrom",    korb_m_sock_recvfrom,    -1);
    korb_class_def_cfn(c, obj, "__sock_sendto",      korb_m_sock_sendto,      -1);
    korb_class_def_cfn(c, obj, "__sock_hostent",     korb_m_sock_hostent,      1);
}
