/*
 * Tracker server, first slice.
 *
 * Done:
 *   read sconfig, listen, hand each connection to a worker thread, answer LIST.
 * Still to write:
 *   GET, createtracker, updatetracker, and dropping peers that stop updating.
 */

#include "netutil.h"

#ifndef _WIN32
#include <dirent.h>
#endif

#define MAXLINE 4096
#define MAX_TRACK_FILES 128

static char torrent_dir[256] = "torrents";
static int server_port = 3490;
static char read_msg[MAXLINE];
static char fname[256];

static void send_str(socket_t sock, const char *text) {
    send_all(sock, text, (int)strlen(text));
}

static int cmd_is(const char *msg, const char *cmd) {
    size_t n = strlen(cmd);
    size_t i;
    for (i = 0; i < n; i++) {
        if (msg[i] == '\0') {
            return 0;
        }
        if (tolower((unsigned char)msg[i]) != tolower((unsigned char)cmd[i])) {
            return 0;
        }
    }
    return msg[n] == '\0' || isspace((unsigned char)msg[n]);
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

static int load_sconfig(const char *dir) {
    char path[512];
    FILE *f;
    char line[256];
    int n = 0;

    if (path_join(path, sizeof path, dir, "sconfig") != 0) {
        return -1;
    }
    f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        trim_inplace(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        n++;
        if (n == 1) {
            server_port = atoi(line);
        } else if (n == 2) {
            snprintf(torrent_dir, sizeof torrent_dir, "%s", line);
        }
    }
    fclose(f);
    if (server_port < 1 || server_port > 65535) {
        logmsg("sconfig has an invalid port\n");
        return -1;
    }
    return 0;
}

/* Collect *.track names. Size and MD5 still have to be read out of each file. */
static int collect_track_names(char names[][256], int cap) {
    int count = 0;
#ifdef _WIN32
    char pattern[512];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*.track", torrent_dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        if (count >= cap) {
            break;
        }
        snprintf(names[count], 256, "%s", fd.cFileName);
        count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(torrent_dir);
    struct dirent *ent;
    if (!dir) {
        return 0;
    }
    while ((ent = readdir(dir)) != NULL) {
        if (!ends_with(ent->d_name, ".track")) {
            continue;
        }
        if (count >= cap) {
            break;
        }
        snprintf(names[count], 256, "%s", ent->d_name);
        count++;
    }
    closedir(dir);
#endif
    return count;
}

static void strip_track_suffix(char *name) {
    size_t n = strlen(name);
    if (n > 6 && strcmp(name + n - 6, ".track") == 0) {
        name[n - 6] = '\0';
    }
}

static void handle_list_req(socket_t sock) {
    char names[MAX_TRACK_FILES][256];
    int n;
    int i;
    char line[320];

    n = collect_track_names(names, MAX_TRACK_FILES);
    snprintf(line, sizeof line, "<$REP LIST %d>\n", n);
    send_str(sock, line);
    for (i = 0; i < n; i++) {
        strip_track_suffix(names[i]);
        /* TODO: replace 0 and TODO with the file size and MD5 from the tracker file. */
        snprintf(line, sizeof line, "<$%d %s 0 TODO>\n", i + 1, names[i]);
        send_str(sock, line);
    }
    send_str(sock, "<$REP LIST END>\n");
    logmsg("list request handled (%d files).\n", n);
}

static void xtrct_fname(const char *msg, const char *delim) {
    const char *p = msg;
    (void)delim;
    fname[0] = '\0';
    while (*p && !isspace((unsigned char)*p)) {
        p++;
    }
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    sscanf(p, "%255s", fname);
}

static void handle_get_req(socket_t sock, const char *name) {
    (void)name;
    /* TODO: send <$REP GET BEGIN>, the tracker file, and <$REP GET END md5>. */
    logmsg("GET is not implemented yet\n");
    send_str(sock, "<$REP GET ferr>\n");
}

static void handle_createtracker_req(socket_t sock) {
    /* TODO: create torrents/<filename>.track if it does not already exist. */
    logmsg("createtracker is not implemented yet\n");
    send_str(sock, "<$createtracker fail>\n");
}

static void handle_updatetracker_req(socket_t sock) {
    /* TODO: add or refresh this peer's byte range and drop silent peers. */
    logmsg("updatetracker is not implemented yet\n");
    send_str(sock, "<$updatetracker fail>\n");
}

static int is_list_cmd(const char *msg) {
    char a[32];
    char b[32];
    a[0] = '\0';
    b[0] = '\0';
    sscanf(msg, "%31s %31s", a, b);
    if (str_ieq(a, "list")) {
        return 1;
    }
    return str_ieq(a, "req") && str_ieq(b, "list");
}

static void peer_handler(socket_t sock) {
    if (recv_line(sock, read_msg, MAXLINE) < 0) {
        return;
    }
    unwrap_msg(read_msg);
    if (read_msg[0] == '\0') {
        return;
    }
    logmsg("request: %s\n", read_msg);
    if (is_list_cmd(read_msg)) {
        handle_list_req(sock);
    } else if (cmd_is(read_msg, "get")) {
        xtrct_fname(read_msg, " ");
        handle_get_req(sock, fname);
    } else if (cmd_is(read_msg, "createtracker")) {
        handle_createtracker_req(sock);
    } else if (cmd_is(read_msg, "updatetracker")) {
        handle_updatetracker_req(sock);
    } else {
        send_str(sock, "<$ERR unknown>\n");
    }
}

THREAD_FN(worker, arg) {
    socket_t sock = (socket_t)(intptr_t)arg;
    set_io_timeout(sock, 5000);
    peer_handler(sock);
    close_socket(sock);
    THREAD_RETURN;
}

int main(int argc, char **argv) {
    socket_t listener;
    char dir[512];

    (void)argc;
    net_start();
    if (load_sconfig(".") != 0) {
        if (!exe_dir(argv ? argv[0] : NULL, dir, sizeof dir) || load_sconfig(dir) != 0) {
            logmsg("No sconfig found. Using port %d and folder '%s'.\n", server_port, torrent_dir);
        } else if (change_dir(dir) != 0) {
            logmsg("Cannot enter %s\n", dir);
            return 1;
        }
    }
    if (ensure_dir(torrent_dir) != 0) {
        logmsg("Cannot create torrent directory '%s'\n", torrent_dir);
        return 1;
    }

    listener = listen_on(server_port);
    if (listener == SOCK_INVALID) {
        return 1;
    }
    logmsg("Tracker listening on port %d\n", server_port);
    logmsg("Torrent directory: %s\n", torrent_dir);
    logmsg("LIST works. GET, createtracker, and updatetracker are still open.\n");

    for (;;) {
        struct sockaddr_in client;
        socklen_t clilen = sizeof client;
        socket_t sock;
        thread_t worker_thread;

        sock = accept(listener, (struct sockaddr *)&client, &clilen);
        if (sock == SOCK_INVALID) {
            continue;
        }
        if (!thread_start(&worker_thread, worker, (void *)(intptr_t)sock)) {
            close_socket(sock);
            continue;
        }
        thread_detach(worker_thread);
    }
    return 0;
}
