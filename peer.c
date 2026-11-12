/*
 * Peer program.
 *
 * Done:
 *   read the two config files and send LIST to the tracker.
 *   listen thread: other peers connect and download file chunks from our
 *   shared folder, max 1024 bytes per request. each connection gets its
 *   own thread.
 *   file transfer: "download <file> <ip> <port>" gets a file from another
 *   peer 1024 bytes at a time and saves it in our shared folder.
 * Still to write:
 *   createtracker, updatetracker, GET, and downloading from several peers
 *   at once using the tracker file.
 *
 * Protocol between peers (the project description does not define it):
 *   request: <GET filename start_byte size>\n   (size is at most 1024)
 *   reply:   <REP CHUNK n>\n followed by n bytes of the file
 *            n can be less than size at the end of the file, 0 means
 *            start_byte is past the end
 *   error:   <GET invalid>\n
 */

#include "netutil.h"

#define MAX_CHUNK 1024

static char tracker_ip[64] = "127.0.0.1";
static int tracker_port = 3490;
static int update_interval = 900;
static int my_port = 5001;
static char shared_dir[256] = "shared";

static int load_key_file(const char *path, char lines[][256], int max_lines) {
    FILE *f = fopen(path, "r");
    char buf[256];
    int n = 0;
    if (!f) {
        return -1;
    }
    while (n < max_lines && fgets(buf, sizeof buf, f)) {
        trim_inplace(buf);
        if (buf[0] == '\0' || buf[0] == '#') {
            continue;
        }
        snprintf(lines[n], 256, "%s", buf);
        n++;
    }
    fclose(f);
    return n;
}

static int load_config_from(const char *dir) {
    char path[512];
    char lines[4][256];
    int n;

    if (path_join(path, sizeof path, dir, "clientThreadConfig.cfg") != 0) {
        return -1;
    }
    n = load_key_file(path, lines, 4);
    if (n < 2) {
        return -1;
    }
    tracker_port = atoi(lines[0]);
    snprintf(tracker_ip, sizeof tracker_ip, "%s", lines[1]);
    if (n >= 3) {
        update_interval = atoi(lines[2]);
    }

    if (path_join(path, sizeof path, dir, "serverThreadConfig.cfg") != 0) {
        return -1;
    }
    n = load_key_file(path, lines, 4);
    if (n < 2) {
        return -1;
    }
    my_port = atoi(lines[0]);
    snprintf(shared_dir, sizeof shared_dir, "%s", lines[1]);
    if (tracker_port < 1 || tracker_port > 65535 || my_port < 1 || my_port > 65535) {
        logmsg("A config file has an invalid port.\n");
        return -1;
    }
    if (update_interval < 1) {
        update_interval = 900;
    }
    return 0;
}

static int exe_dir(const char *argv0, char *out, size_t cap) {
    char *slash;
    char *other;
    if (!argv0 || !argv0[0]) {
        return 0;
    }
    snprintf(out, cap, "%s", argv0);
    slash = strrchr(out, '\\');
    other = strrchr(out, '/');
    if (other && (!slash || other > slash)) {
        slash = other;
    }
    if (!slash) {
        return 0;
    }
    *slash = '\0';
    return out[0] != '\0';
}

static int load_config(const char *argv0) {
    char dir[512];
    if (load_config_from(".") == 0) {
        return 0;
    }
    if (exe_dir(argv0, dir, sizeof dir) && load_config_from(dir) == 0) {
        if (change_dir(dir) != 0) {
            logmsg("Cannot enter %s\n", dir);
            return -1;
        }
        return 0;
    }
    logmsg("Missing clientThreadConfig.cfg or serverThreadConfig.cfg.\n");
    logmsg("Run this program from the peer1, peer2, or peer3 folder.\n");
    return -1;
}

static int tracker_exchange(const char *msg, char *reply, int cap) {
    socket_t s;
    int n = 0;
    if (cap <= 0) {
        return -1;
    }
    reply[0] = '\0';
    s = connect_to(tracker_ip, tracker_port);
    if (s == SOCK_INVALID) {
        logmsg("Cannot connect to tracker %s:%d\n", tracker_ip, tracker_port);
        return -1;
    }
    logmsg(">> %s", msg);
    if (send_all(s, msg, (int)strlen(msg)) < 0) {
        close_socket(s);
        return -1;
    }
    while (n < cap - 1) {
        int r = recv(s, reply + n, cap - 1 - n, 0);
        if (r <= 0) {
            break;
        }
        n += r;
    }
    reply[n] = '\0';
    close_socket(s);
    logmsg("<< %s%s", reply, (n > 0 && reply[n - 1] == '\n') ? "" : "\n");
    return n;
}

static void do_list(void) {
    char reply[8192];
    tracker_exchange("<$REQ LIST>\n", reply, (int)sizeof reply);
}

/* ---------------- listen thread (serving chunks to other peers) ---------------- */

/* file names come from other peers, so no slashes allowed (no ../ tricks) */
static int safe_name(const char *name) {
    if (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

/* one thread per connected peer. it can ask for many chunks on the same connection */
THREAD_FN(serve_peer, arg) {
    socket_t s = (socket_t)(intptr_t)arg;
    char line[1024];
    char reply[64 + MAX_CHUNK];

    while (recv_line(s, line, sizeof line) >= 0) {
        char cmd[16];
        char name[256];
        long start;
        int size;
        char path[512];
        FILE *f;
        int got;
        int hlen;

        unwrap_msg(line);
        if (sscanf(line, "%15s %255s %ld %d", cmd, name, &start, &size) != 4 ||
            !str_ieq(cmd, "GET") || !safe_name(name) || start < 0) {
            send_all(s, "<GET invalid>\n", 14);
            continue;
        }
        if (size <= 0 || size > MAX_CHUNK) {
            logmsg("Rejected request for %d bytes of %s (max is %d)\n", size, name, MAX_CHUNK);
            send_all(s, "<GET invalid>\n", 14);
            continue;
        }
        if (path_join(path, sizeof path, shared_dir, name) != 0 || !(f = fopen(path, "rb"))) {
            send_all(s, "<GET invalid>\n", 14);
            continue;
        }

        /* header and data go out in one send, two small sends in a row
           get slowed down by Nagle's algorithm */
        got = 0;
        if (fseek(f, start, SEEK_SET) == 0) {
            got = (int)fread(reply + 64, 1, (size_t)size, f);
        }
        fclose(f);
        hlen = snprintf(reply, 64, "<REP CHUNK %d>\n", got);
        memmove(reply + hlen, reply + 64, (size_t)got);
        if (send_all(s, reply, hlen + got) < 0) {
            break;
        }
    }
    close_socket(s);
    THREAD_RETURN;
}

/* welcoming socket: waits for other peers and gives each one its own thread */
THREAD_FN(listen_thread, arg) {
    socket_t ls = listen_on(my_port);
    (void)arg;
    if (ls == SOCK_INVALID) {
        logmsg("Cannot listen on port %d, is another peer using it?\n", my_port);
        THREAD_RETURN;
    }
    logmsg("Listening for other peers on port %d\n", my_port);

    for (;;) {
        struct sockaddr_in from;
        socklen_t len = sizeof from;
        thread_t t;
        socket_t c = accept(ls, (struct sockaddr *)&from, &len);
        if (c == SOCK_INVALID) {
            continue;
        }
        logmsg("Peer %s connected\n", inet_ntoa(from.sin_addr));
        if (!thread_start(&t, serve_peer, (void *)(intptr_t)c)) {
            logmsg("Could not start a thread for that peer\n");
            close_socket(c);
            continue;
        }
        thread_detach(t);
    }
    THREAD_RETURN;
}

/* ---------------- file transfer (downloading from another peer) ---------------- */

/* download <file> <ip> <port>: asks the other peer for the file 1024 bytes
   at a time and saves it in our shared folder */
static void do_download(const char *name, const char *ip, int port) {
    socket_t s;
    char path[512];
    char line[256];
    char buf[MAX_CHUNK];
    FILE *out;
    long start = 0;
    int ok = 0;

    if (!safe_name(name) || port < 1 || port > 65535) {
        logmsg("Usage: download <filename> <ip> <port>\n");
        return;
    }
    if (path_join(path, sizeof path, shared_dir, name) != 0) {
        return;
    }
    s = connect_to(ip, port);
    if (s == SOCK_INVALID) {
        logmsg("Cannot connect to peer %s:%d\n", ip, port);
        return;
    }
    out = fopen(path, "wb");
    if (!out) {
        logmsg("Cannot create %s\n", path);
        close_socket(s);
        return;
    }

    logmsg("File %s requested from %s %d\n", name, ip, port);
    for (;;) {
        char req[512];
        int n;
        snprintf(req, sizeof req, "<GET %s %ld %d>\n", name, start, MAX_CHUNK);
        if (send_all(s, req, (int)strlen(req)) < 0 || recv_line(s, line, sizeof line) < 0) {
            logmsg("Lost connection to %s %d\n", ip, port);
            break;
        }
        if (sscanf(line, "<REP CHUNK %d>", &n) != 1 || n < 0 || n > MAX_CHUNK) {
            logmsg("Peer said: %s (does it have %s?)\n", line, name);
            break;
        }
        if (n == 0) {
            ok = 1;     /* past the end of the file, we have all of it */
            break;
        }
        if (recv_all(s, buf, n) < 0 || fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            logmsg("Error while receiving %s\n", name);
            break;
        }
        logmsg("Downloading %ld to %ld bytes of %s from %s %d\n", start, start + n - 1, name, ip, port);
        start += n;
        if (n < MAX_CHUNK) {
            ok = 1;     /* short chunk means that was the last one */
            break;
        }
    }
    fclose(out);
    close_socket(s);

    if (ok) {
        logmsg("File %s download complete (%ld bytes)\n", name, start);
    } else {
        remove(path);   /* don't leave half a file behind */
        logmsg("Download of %s failed\n", name);
    }
}

/* ---------------- commands ---------------- */

static int is_list_command(const char *line) {
    char a[32];
    char b[32];
    a[0] = '\0';
    b[0] = '\0';
    if (sscanf(line, "%31s %31s", a, b) < 1) {
        return 0;
    }
    if (str_ieq(a, "list")) {
        return 1;
    }
    return str_ieq(a, "req") && str_ieq(b, "list");
}

static void print_help(void) {
    logmsg(
        "Working now:\n"
        "  list\n"
        "  download <filename> <ip> <port>   (get a file from another peer)\n"
        "  listening on port %d so other peers can download from us\n"
        "Not written yet:\n"
        "  get <filename.track>\n"
        "  createtracker <file> <size> <description> <md5> [ip port]\n"
        "  updatetracker <file> <start> <end> [ip port]\n"
        "  quit\n",
        my_port);
}

static int handle_command(const char *input) {
    char line[1024];
    char cmd[64];

    snprintf(line, sizeof line, "%s", input);
    unwrap_msg(line);
    if (line[0] == '\0') {
        return 0;
    }
    cmd[0] = '\0';
    sscanf(line, "%63s", cmd);
    if (str_ieq(cmd, "quit") || str_ieq(cmd, "exit")) {
        return 1;
    }
    if (str_ieq(cmd, "help")) {
        print_help();
        return 0;
    }
    if (is_list_command(line)) {
        do_list();
        return 0;
    }
    if (str_ieq(cmd, "download")) {
        char name[256];
        char ip[64];
        int port = 0;
        if (sscanf(line, "%*s %255s %63s %d", name, ip, &port) != 3) {
            logmsg("Usage: download <filename> <ip> <port>\n");
            return 0;
        }
        do_download(name, ip, port);
        return 0;
    }
    if (str_ieq(cmd, "get") || str_ieq(cmd, "createtracker") || str_ieq(cmd, "updatetracker")) {
        logmsg("%s is not implemented yet.\n", cmd);
        return 0;
    }
    logmsg("Unknown command. Type help.\n");
    return 0;
}

static void print_banner(void) {
    logmsg("Peer ready.\n");
    logmsg("  Tracker: %s:%d\n", tracker_ip, tracker_port);
    logmsg("  Listen port: %d\n", my_port);
    logmsg("  Shared folder: %s\n", shared_dir);
    logmsg("  Update interval: %d seconds (not scheduled yet)\n", update_interval);
    logmsg("Type help for commands.\n");
}

int main(int argc, char **argv) {
    net_start();
    if (load_config(argv ? argv[0] : NULL) != 0) {
        return 1;
    }
    ensure_dir(shared_dir);
    ensure_dir("cache");

    if (argc > 1) {
        char line[1024] = "";
        int i;
        for (i = 1; i < argc; i++) {
            size_t used = strlen(line);
            if (used + 2 >= sizeof line) {
                break;
            }
            if (i > 1) {
                line[used++] = ' ';
                line[used] = '\0';
            }
            snprintf(line + strlen(line), sizeof line - strlen(line), "%s", argv[i]);
        }
        handle_command(line);
        return 0;
    }

    print_banner();
    {
        thread_t t;
        if (thread_start(&t, listen_thread, NULL)) {
            thread_detach(t);
        } else {
            logmsg("Could not start the listen thread\n");
        }
    }
    sleep_ms(100);   /* let the listen message print before the prompt */

    for (;;) {
        char line[1024];
        logmsg("peer> ");
        if (!fgets(line, sizeof line, stdin)) {
            break;
        }
        if (handle_command(line)) {
            break;
        }
    }
    return 0;
}
