/*
 * Peer program.
 *
 * Talks to the tracker with LIST and GET.
 * Saves downloaded .track files into the local cache.
 */

#include "netutil.h"

#define REPLY_CAP (256 * 1024)

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
    char reply[REPLY_CAP];
    tracker_exchange("<$REQ LIST>\n", reply, (int)sizeof reply);
}

static int extract_get_body(const char *text, char **out_body, size_t *out_len,
                            char *out_md5, size_t md5_cap) {
    const char *b;
    const char *e;
    const char *body;
    const char *md5_at;
    size_t blen;
    *out_body = NULL;
    *out_len = 0;
    if (out_md5 && md5_cap) {
        out_md5[0] = '\0';
    }
    b = strstr(text, "REP GET BEGIN");
    if (!b) {
        return -1;
    }
    body = strchr(b, '\n');
    if (!body) {
        return -1;
    }
    body++;
    e = strstr(body, "<$REP GET END");
    if (!e) {
        e = strstr(body, "<REP GET END");
    }
    if (!e) {
        return -1;
    }
    blen = (size_t)(e - body);
    {
        char *copy = (char *)malloc(blen + 1);
        if (!copy) {
            return -1;
        }
        memcpy(copy, body, blen);
        copy[blen] = '\0';
        *out_body = copy;
        *out_len = blen;
    }
    md5_at = strstr(e, "END");
    if (out_md5 && md5_cap && md5_at) {
        md5_at += 3;
        while (*md5_at && isspace((unsigned char)*md5_at)) {
            md5_at++;
        }
        sscanf(md5_at, "%32s", out_md5);
        {
            size_t n = strlen(out_md5);
            if (n > 0 && out_md5[n - 1] == '>') {
                out_md5[n - 1] = '\0';
            }
        }
    }
    return 0;
}

static void do_get(const char *line) {
    char name[256] = "";
    char msg[512];
    char *reply;
    char *body = NULL;
    size_t body_len = 0;
    char claimed[40] = "";
    char actual[33];
    char cache_path[512];
    int n;

    if (sscanf(line, "%*s %255s", name) != 1 || name[0] == '\0') {
        logmsg("Usage: get <filename.track>\n");
        return;
    }
    if (!ends_with(name, ".track")) {
        size_t len = strlen(name);
        if (len + 6 < sizeof name) {
            memcpy(name + len, ".track", 7);
        }
    }

    reply = (char *)malloc(REPLY_CAP);
    if (!reply) {
        return;
    }
    snprintf(msg, sizeof msg, "<$GET %s>\n", name);
    n = tracker_exchange(msg, reply, REPLY_CAP);
    if (n < 0) {
        free(reply);
        return;
    }
    if (strstr(reply, "ferr") != NULL && strstr(reply, "REP GET BEGIN") == NULL) {
        logmsg("GET failed for %s\n", name);
        free(reply);
        return;
    }
    if (extract_get_body(reply, &body, &body_len, claimed, sizeof claimed) != 0) {
        logmsg("Malformed GET reply for %s\n", name);
        free(reply);
        return;
    }
    free(reply);

    md5_buffer(body, body_len, actual);
    if (claimed[0] && strcmp(claimed, actual) != 0) {
        logmsg("GET MD5 mismatch for %s (got %s, expected %s)\n", name, actual, claimed);
        free(body);
        return;
    }

    ensure_dir("cache");
    if (path_join(cache_path, sizeof cache_path, "cache", name) != 0 ||
        write_file_all(cache_path, body, body_len) != 0) {
        logmsg("Cannot save tracker file to cache\n");
        free(body);
        return;
    }
    free(body);
    logmsg("Saved tracker file to %s (MD5 ok)\n", cache_path);
}

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
        "Commands:\n"
        "  list\n"
        "  get <filename.track>\n"
        "  quit\n");
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
    if (str_ieq(cmd, "get")) {
        do_get(line);
        return 0;
    }
    logmsg("Unknown command. Type help.\n");
    return 0;
}

static void print_banner(void) {
    logmsg("Peer ready.\n");
    logmsg("  Tracker: %s:%d\n", tracker_ip, tracker_port);
    logmsg("  Shared folder: %s\n", shared_dir);
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
