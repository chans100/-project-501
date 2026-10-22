#ifndef NETUTIL_H
#define NETUTIL_H

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

#endif
