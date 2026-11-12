#ifndef NETUTIL_H
#define NETUTIL_H

/* MinGW + MSVCRT needs this so %lld works in printf/scanf. */
#ifdef __MINGW32__
#ifndef __USE_MINGW_ANSI_STDIO
#define __USE_MINGW_ANSI_STDIO 1
#endif
#endif

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <direct.h>
typedef SOCKET socket_t;
#define SOCK_INVALID INVALID_SOCKET
#define close_socket(s) closesocket(s)
typedef HANDLE thread_t;
#define THREAD_FN(name, arg) static DWORD WINAPI name(LPVOID arg)
#define THREAD_RETURN return 0
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_t;
#define SOCK_INVALID (-1)
#define close_socket(s) close(s)
typedef pthread_t thread_t;
#define THREAD_FN(name, arg) static void *name(void *arg)
#define THREAD_RETURN return NULL
#endif

static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

static void sock_perror(const char *what) {
#ifdef _WIN32
    logmsg("%s (winsock error %d)\n", what, WSAGetLastError());
#else
    logmsg("%s: %s\n", what, strerror(errno));
#endif
}

static void net_start(void) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logmsg("WSAStartup failed\n");
        exit(1);
    }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
}

static int ensure_dir(const char *path) {
#ifdef _WIN32
    if (_mkdir(path) == 0 || errno == EEXIST) {
        return 0;
    }
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
#endif
    return -1;
}

static int change_dir(const char *path) {
#ifdef _WIN32
    return _chdir(path);
#else
    return chdir(path);
#endif
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

static int str_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void trim_inplace(char *s) {
    char *start = s;
    size_t n;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

/* Drop an optional <$...> wrapper so both manual and protocol text match. */
static void unwrap_msg(char *s) {
    size_t n;
    trim_inplace(s);
    n = strlen(s);
    if (n >= 2 && s[0] == '<' && s[1] == '$') {
        memmove(s, s + 2, n - 1);
    } else if (s[0] == '<') {
        memmove(s, s + 1, n);
    }
    n = strlen(s);
    if (n > 0 && s[n - 1] == '>') {
        s[n - 1] = '\0';
    }
    trim_inplace(s);
}

static int path_join(char *out, size_t cap, const char *a, const char *b) {
    int n = snprintf(out, cap, "%s/%s", a, b);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int ends_with(const char *s, const char *suffix) {
    size_t n, m;
    if (!s || !suffix) {
        return 0;
    }
    n = strlen(s);
    m = strlen(suffix);
    if (m > n) {
        return 0;
    }
    return strcmp(s + n - m, suffix) == 0;
}

static int set_nonblock(socket_t s, int on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (on) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(s, F_SETFL, flags);
#endif
}

static int wait_writable(socket_t s, int timeout_ms) {
    fd_set wset;
    struct timeval tv;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    return select(0, NULL, &wset, NULL, &tv) > 0;
#else
    return select(s + 1, NULL, &wset, NULL, &tv) > 0;
#endif
}

static void set_io_timeout(socket_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&t, sizeof t);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

static socket_t connect_to(const char *host, int port) {
    char portstr[16];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p;
    socket_t s = SOCK_INVALID;

    snprintf(portstr, sizeof portstr, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        return SOCK_INVALID;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        int rc;
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == SOCK_INVALID) {
            continue;
        }
        set_nonblock(s, 1);
        rc = connect(s, p->ai_addr, p->ai_addrlen);
        if (rc != 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEALREADY) {
                close_socket(s);
                s = SOCK_INVALID;
                continue;
            }
#else
            if (errno != EINPROGRESS && errno != EALREADY) {
                close_socket(s);
                s = SOCK_INVALID;
                continue;
            }
#endif
            if (!wait_writable(s, 3000)) {
                close_socket(s);
                s = SOCK_INVALID;
                continue;
            }
            {
                int soerr = 0;
                socklen_t sl = sizeof soerr;
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &sl) < 0 || soerr != 0) {
                    close_socket(s);
                    s = SOCK_INVALID;
                    continue;
                }
            }
        }
        set_nonblock(s, 0);
        set_io_timeout(s, 5000);
        break;
    }
    freeaddrinfo(res);
    return s;
}

static int send_all(socket_t s, const void *buf, int len) {
    const char *p = (const char *)buf;
    int off = 0;
    while (off < len) {
        int n = send(s, p + off, len - off, 0);
        if (n <= 0) {
            return -1;
        }
        off += n;
    }
    return off;
}

static int recv_all(socket_t s, void *buf, int len) {
    char *p = (char *)buf;
    int off = 0;
    while (off < len) {
        int n = recv(s, p + off, len - off, 0);
        if (n <= 0) {
            return -1;
        }
        off += n;
    }
    return off;
}

/* Read one line, stripping CR/LF. Returns the length, or -1 on EOF/error. */
static int recv_line(socket_t s, char *buf, int cap) {
    int n = 0;
    if (cap <= 1) {
        return -1;
    }
    while (n < cap - 1) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r <= 0) {
            if (n == 0) {
                return -1;
            }
            break;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            buf[n] = '\0';
            return n;
        }
        buf[n++] = c;
    }
    buf[n] = '\0';
    if (n == cap - 1) {
        char c;
        while (recv(s, &c, 1, 0) == 1 && c != '\n') {
        }
    }
    return n;
}

static socket_t listen_on(int port) {
    socket_t s;
    struct sockaddr_in addr;
    int opt = 1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCK_INVALID) {
        sock_perror("socket");
        return SOCK_INVALID;
    }
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof opt);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        sock_perror("bind");
        close_socket(s);
        return SOCK_INVALID;
    }
    if (listen(s, 16) != 0) {
        sock_perror("listen");
        close_socket(s);
        return SOCK_INVALID;
    }
    return s;
}

#ifdef _WIN32
static int thread_start(thread_t *out, LPTHREAD_START_ROUTINE fn, void *arg) {
    *out = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return *out != NULL;
}
static void thread_detach(thread_t t) {
    if (t) {
        CloseHandle(t);
    }
}
static void thread_join(thread_t t) {
    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
}
#else
static int thread_start(thread_t *out, void *(*fn)(void *), void *arg) {
    return pthread_create(out, NULL, fn, arg) == 0;
}
static void thread_detach(thread_t t) {
    pthread_detach(t);
}
static void thread_join(thread_t t) {
    pthread_join(t, NULL);
}
#endif

#ifdef _WIN32
typedef CRITICAL_SECTION mutex_t;
static void mutex_init(mutex_t *m) { InitializeCriticalSection(m); }
static void mutex_lock(mutex_t *m) { EnterCriticalSection(m); }
static void mutex_unlock(mutex_t *m) { LeaveCriticalSection(m); }
#else
typedef pthread_mutex_t mutex_t;
static void mutex_init(mutex_t *m) { pthread_mutex_init(m, NULL); }
static void mutex_lock(mutex_t *m) { pthread_mutex_lock(m); }
static void mutex_unlock(mutex_t *m) { pthread_mutex_unlock(m); }
#endif

/* Compact MD5 (public-domain style) for tracker/file checksums. */
typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    unsigned char buffer[64];
} md5_ctx;

static void md5_transform(uint32_t state[4], const unsigned char block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    int i;
    for (i = 0; i < 16; i++) {
        x[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
    }
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))
#define ROT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define STEP(f, a, b, c, d, x, s, ac) \
    do { (a) += f((b), (c), (d)) + (x) + (uint32_t)(ac); (a) = ROT((a), (s)); (a) += (b); } while (0)
    STEP(F, a, b, c, d, x[0], 7, 0xd76aa478); STEP(F, d, a, b, c, x[1], 12, 0xe8c7b756);
    STEP(F, c, d, a, b, x[2], 17, 0x242070db); STEP(F, b, c, d, a, x[3], 22, 0xc1bdceee);
    STEP(F, a, b, c, d, x[4], 7, 0xf57c0faf); STEP(F, d, a, b, c, x[5], 12, 0x4787c62a);
    STEP(F, c, d, a, b, x[6], 17, 0xa8304613); STEP(F, b, c, d, a, x[7], 22, 0xfd469501);
    STEP(F, a, b, c, d, x[8], 7, 0x698098d8); STEP(F, d, a, b, c, x[9], 12, 0x8b44f7af);
    STEP(F, c, d, a, b, x[10], 17, 0xffff5bb1); STEP(F, b, c, d, a, x[11], 22, 0x895cd7be);
    STEP(F, a, b, c, d, x[12], 7, 0x6b901122); STEP(F, d, a, b, c, x[13], 12, 0xfd987193);
    STEP(F, c, d, a, b, x[14], 17, 0xa679438e); STEP(F, b, c, d, a, x[15], 22, 0x49b40821);
    STEP(G, a, b, c, d, x[1], 5, 0xf61e2562); STEP(G, d, a, b, c, x[6], 9, 0xc040b340);
    STEP(G, c, d, a, b, x[11], 14, 0x265e5a51); STEP(G, b, c, d, a, x[0], 20, 0xe9b6c7aa);
    STEP(G, a, b, c, d, x[5], 5, 0xd62f105d); STEP(G, d, a, b, c, x[10], 9, 0x02441453);
    STEP(G, c, d, a, b, x[15], 14, 0xd8a1e681); STEP(G, b, c, d, a, x[4], 20, 0xe7d3fbc8);
    STEP(G, a, b, c, d, x[9], 5, 0x21e1cde6); STEP(G, d, a, b, c, x[14], 9, 0xc33707d6);
    STEP(G, c, d, a, b, x[3], 14, 0xf4d50d87); STEP(G, b, c, d, a, x[8], 20, 0x455a14ed);
    STEP(G, a, b, c, d, x[13], 5, 0xa9e3e905); STEP(G, d, a, b, c, x[2], 9, 0xfcefa3f8);
    STEP(G, c, d, a, b, x[7], 14, 0x676f02d9); STEP(G, b, c, d, a, x[12], 20, 0x8d2a4c8a);
    STEP(H, a, b, c, d, x[5], 4, 0xfffa3942); STEP(H, d, a, b, c, x[8], 11, 0x8771f681);
    STEP(H, c, d, a, b, x[11], 16, 0x6d9d6122); STEP(H, b, c, d, a, x[14], 23, 0xfde5380c);
    STEP(H, a, b, c, d, x[1], 4, 0xa4beea44); STEP(H, d, a, b, c, x[4], 11, 0x4bdecfa9);
    STEP(H, c, d, a, b, x[7], 16, 0xf6bb4b60); STEP(H, b, c, d, a, x[10], 23, 0xbebfbc70);
    STEP(H, a, b, c, d, x[13], 4, 0x289b7ec6); STEP(H, d, a, b, c, x[0], 11, 0xeaa127fa);
    STEP(H, c, d, a, b, x[3], 16, 0xd4ef3085); STEP(H, b, c, d, a, x[6], 23, 0x04881d05);
    STEP(H, a, b, c, d, x[9], 4, 0xd9d4d039); STEP(H, d, a, b, c, x[12], 11, 0xe6db99e5);
    STEP(H, c, d, a, b, x[15], 16, 0x1fa27cf8); STEP(H, b, c, d, a, x[2], 23, 0xc4ac5665);
    STEP(I, a, b, c, d, x[0], 6, 0xf4292244); STEP(I, d, a, b, c, x[7], 10, 0x432aff97);
    STEP(I, c, d, a, b, x[14], 15, 0xab9423a7); STEP(I, b, c, d, a, x[5], 21, 0xfc93a039);
    STEP(I, a, b, c, d, x[12], 6, 0x655b59c3); STEP(I, d, a, b, c, x[3], 10, 0x8f0ccc92);
    STEP(I, c, d, a, b, x[10], 15, 0xffeff47d); STEP(I, b, c, d, a, x[1], 21, 0x85845dd1);
    STEP(I, a, b, c, d, x[8], 6, 0x6fa87e4f); STEP(I, d, a, b, c, x[15], 10, 0xfe2ce6e0);
    STEP(I, c, d, a, b, x[6], 15, 0xa3014314); STEP(I, b, c, d, a, x[13], 21, 0x4e0811a1);
    STEP(I, a, b, c, d, x[4], 6, 0xf7537e82); STEP(I, d, a, b, c, x[11], 10, 0xbd3af235);
    STEP(I, c, d, a, b, x[2], 15, 0x2ad7d2bb); STEP(I, b, c, d, a, x[9], 21, 0xeb86d391);
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
#undef F
#undef G
#undef H
#undef I
#undef ROT
#undef STEP
}

static void md5_init(md5_ctx *ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
}

static void md5_update(md5_ctx *ctx, const unsigned char *input, size_t len) {
    size_t i, idx = (size_t)((ctx->count[0] >> 3) & 0x3F);
    uint32_t part = (uint32_t)(len << 3);
    ctx->count[0] += part;
    if (ctx->count[0] < part) {
        ctx->count[1]++;
    }
    ctx->count[1] += (uint32_t)(len >> 29);
    for (i = 0; i < len; i++) {
        ctx->buffer[idx++] = input[i];
        if (idx == 64) {
            md5_transform(ctx->state, ctx->buffer);
            idx = 0;
        }
    }
}

static void md5_final(unsigned char digest[16], md5_ctx *ctx) {
    unsigned char bits[8];
    size_t idx = (size_t)((ctx->count[0] >> 3) & 0x3F);
    size_t pad = (idx < 56) ? (56 - idx) : (120 - idx);
    static const unsigned char padding[64] = {0x80};
    int i;
    for (i = 0; i < 8; i++) {
        bits[i] = (unsigned char)((ctx->count[i >> 2] >> ((i & 3) * 8)) & 0xff);
    }
    md5_update(ctx, padding, pad);
    md5_update(ctx, bits, 8);
    for (i = 0; i < 16; i++) {
        digest[i] = (unsigned char)((ctx->state[i >> 2] >> ((i & 3) * 8)) & 0xff);
    }
}

static void md5_hex(const unsigned char digest[16], char out[33]) {
    static const char *hex = "0123456789abcdef";
    int i;
    for (i = 0; i < 16; i++) {
        out[i * 2] = hex[(digest[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[digest[i] & 0xf];
    }
    out[32] = '\0';
}

static int md5_buffer(const void *data, size_t len, char out_hex[33]) {
    md5_ctx ctx;
    unsigned char dig[16];
    md5_init(&ctx);
    md5_update(&ctx, (const unsigned char *)data, len);
    md5_final(dig, &ctx);
    md5_hex(dig, out_hex);
    return 0;
}

static int md5_file(const char *path, char out_hex[33], long long *out_size) {
    FILE *f = fopen(path, "rb");
    md5_ctx ctx;
    unsigned char buf[4096];
    unsigned char dig[16];
    long long total = 0;
    size_t n;
    if (!f) {
        return -1;
    }
    md5_init(&ctx);
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        md5_update(&ctx, buf, n);
        total += (long long)n;
    }
    fclose(f);
    md5_final(dig, &ctx);
    md5_hex(dig, out_hex);
    if (out_size) {
        *out_size = total;
    }
    return 0;
}

static long long file_size_of(const char *path) {
    FILE *f = fopen(path, "rb");
    long long sz;
    if (!f) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    sz = (long long)ftell(f);
    fclose(f);
    return sz;
}

static int read_file_all(const char *path, char **out, size_t *out_len) {
    FILE *f;
    long long sz;
    char *buf;
    size_t n;
    *out = NULL;
    *out_len = 0;
    sz = file_size_of(path);
    if (sz < 0) {
        return -1;
    }
    f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return -1;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return 0;
}

static int write_file_all(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static int get_local_ip(char *out, size_t cap) {
    socket_t s;
    struct sockaddr_in peer;
    struct sockaddr_in local;
    socklen_t len = sizeof local;
    if (cap < 8) {
        return -1;
    }
    snprintf(out, cap, "127.0.0.1");
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_INVALID) {
        return 0;
    }
    memset(&peer, 0, sizeof peer);
    peer.sin_family = AF_INET;
    peer.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &peer.sin_addr);
    if (connect(s, (struct sockaddr *)&peer, sizeof peer) == 0) {
        if (getsockname(s, (struct sockaddr *)&local, &len) == 0) {
            inet_ntop(AF_INET, &local.sin_addr, out, (socklen_t)cap);
        }
    }
    close_socket(s);
    return 0;
}

#endif
