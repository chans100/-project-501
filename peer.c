/*
 * peer.c
 * CS 4390 P2P project - peer program
 *
 * Done so far:
 *   - reads clientThreadConfig.cfg and serverThreadConfig.cfg
 *   - peer server thread: listens for other peers and sends them file
 *     chunks from the shared folder (max 1024 bytes per request)
 *
 * Protocol between peers (our choice, the project description doesn't define it):
 *   request: <GET filename start_byte size>\n
 *   reply:   <REP CHUNK size>\n followed by size raw bytes
 *   error:   <GET invalid>\n   (bad request, or size over 1024)
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_CHUNK 1024

// values from the config files
int tracker_port;
char tracker_ip[64];
int update_interval = 900;
int my_port;
char shared_dir[256];

// reads the non-empty lines of a config file into lines[], returns how many
int read_cfg_lines(const char *path, char lines[][256], int max)
{
    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    char buf[256];
    int n = 0;
    while (n < max && fgets(buf, sizeof(buf), fp) != NULL) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (buf[0] == '\0')
            continue;
        strcpy(lines[n++], buf);
    }
    fclose(fp);
    return n;
}

// clientThreadConfig.cfg: tracker port, tracker ip, ..., refresh interval (last line)
// serverThreadConfig.cfg: listen port, ..., shared folder (last line)
int load_configs(void)
{
    char lines[10][256];

    int n = read_cfg_lines("clientThreadConfig.cfg", lines, 10);
    if (n < 3) {
        printf("could not read clientThreadConfig.cfg\n");
        return -1;
    }
    tracker_port = atoi(lines[0]);
    strcpy(tracker_ip, lines[1]);
    update_interval = atoi(lines[n - 1]);

    n = read_cfg_lines("serverThreadConfig.cfg", lines, 10);
    if (n < 2) {
        printf("could not read serverThreadConfig.cfg\n");
        return -1;
    }
    my_port = atoi(lines[0]);
    strcpy(shared_dir, lines[n - 1]);

    if (tracker_port <= 0 || tracker_port > 65535 || my_port <= 0 || my_port > 65535) {
        printf("bad port number in config files\n");
        return -1;
    }
    if (update_interval <= 0)
        update_interval = 900;
    return 0;
}

// read one line ending in '\n' (TCP is a byte stream, one read() is not one message)
static int srv_read_line(int sock, char *buf, int max)
{
    int count = 0;
    char c;
    while (count < max - 1) {
        int n = recv(sock, &c, 1, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        buf[count++] = c;
        if (c == '\n')
            break;
    }
    buf[count] = '\0';
    return count;
}

// send() may not send everything at once, so loop until it's all out
static int srv_send_all(int sock, const char *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = send(sock, buf + total, len - total, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        total += n;
    }
    return 0;
}

// handles one connection from another peer.
// request: <GET filename start_byte size>\n
// reply:   <REP CHUNK size>\n followed by size raw bytes, or <GET invalid>\n
void *serve_peer(void *arg)
{
    int sock = *(int *)arg;
    free(arg);

    char line[2048];
    char buf[MAX_CHUNK];

    while (srv_read_line(sock, line, sizeof(line)) > 0) {
        char fname[256];
        long start;
        int size;

        if (sscanf(line, "<GET %255s %ld %d>", fname, &start, &size) != 3 ||
            strchr(fname, '/') != NULL) {
            srv_send_all(sock, "<GET invalid>\n", 14);
            continue;
        }

        // never send more than 1024 bytes for one request
        if (size <= 0 || size > MAX_CHUNK || start < 0) {
            printf("rejected request for %d bytes of %s (max %d)\n", size, fname, MAX_CHUNK);
            srv_send_all(sock, "<GET invalid>\n", 14);
            continue;
        }

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", shared_dir, fname);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            srv_send_all(sock, "<GET invalid>\n", 14);
            continue;
        }
        int got = pread(fd, buf, size, start);
        close(fd);
        if (got != size) {
            srv_send_all(sock, "<GET invalid>\n", 14);
            continue;
        }

        // header + data in one send (two small sends get slowed down by Nagle)
        char reply[64 + MAX_CHUNK];
        int hlen = sprintf(reply, "<REP CHUNK %d>\n", size);
        memcpy(reply + hlen, buf, size);
        if (srv_send_all(sock, reply, hlen + size) < 0)
            break;
    }
    close(sock);
    return NULL;
}

// welcoming socket for other peers. each connection gets its own thread
void *server_thread(void *arg)
{
    (void)arg;
    int sockid = socket(AF_INET, SOCK_STREAM, 0);
    if (sockid < 0) {
        printf("peer server socket cannot be created\n");
        exit(1);
    }
    int yes = 1;
    setsockopt(sockid, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(my_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sockid, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("peer server bind failure on port %d (%s)\n", my_port, strerror(errno));
        exit(1);
    }
    if (listen(sockid, 20) < 0) {
        printf("peer server cannot listen\n");
        exit(1);
    }
    printf("peer server listening on port %d, sharing %s/\n", my_port, shared_dir);

    while (1) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int s = accept(sockid, (struct sockaddr *)&client, &len);
        if (s < 0)
            continue;

        char ip[64];
        inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
        printf("peer connected from %s\n", ip);

        int *p = malloc(sizeof(int));
        if (p == NULL) {
            close(s);
            continue;
        }
        *p = s;
        pthread_t tid;
        if (pthread_create(&tid, NULL, serve_peer, p) != 0) {
            close(s);
            free(p);
            continue;
        }
        pthread_detach(tid);
    }
    return NULL;
}


// gets the folder this peer runs from (peer1, peer2, ...) so the config
// files are found even if started from the root folder as ./peer1/peer
void go_to_peer_folder(const char *prog)
{
    char path[512];
    strncpy(path, prog, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char *slash = strrchr(path, '/');
    if (slash != NULL) {
        *slash = '\0';
        if (chdir(path) != 0) {
            printf("cannot open folder %s\n", path);
            exit(1);
        }
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    go_to_peer_folder(argv[0]);

    setvbuf(stdout, NULL, _IOLBF, 0);   // print logs right away
    signal(SIGPIPE, SIG_IGN);           // don't crash if another peer disconnects

    if (load_configs() < 0)
        exit(1);
    printf("config loaded: tracker %s:%d, refresh every %d secs\n",
           tracker_ip, tracker_port, update_interval);

    // start the server thread so other peers can download from us
    pthread_t server_tid;
    if (pthread_create(&server_tid, NULL, server_thread, NULL) != 0) {
        printf("could not start server thread\n");
        exit(1);
    }
    pthread_detach(server_tid);

    // TODO (client side): createtracker / updatetracker / REQ LIST / GET
    // with the tracker, and the download threads, go here

    while (1)
        pause();   // keep running so the server thread keeps serving
    return 0;
}
