// SPDX-License-Identifier: LGPL-2.1-or-later
#include <stdio.h>
#include <string.h>

extern int cmd_ws_echo(int argc, char **argv);

typedef struct Subcommand {
    const char *name;
    int       (*fn)(int argc, char **argv);
    const char *summary;
} Subcommand;

static const Subcommand g_cmds[] = {
    { "ws-echo", cmd_ws_echo, "Run a WebSocket echo server" },
};
static const size_t g_cmds_count = sizeof(g_cmds) / sizeof(g_cmds[0]);

static void print_help(FILE *out)
{
    fprintf(out, "usage: ffnet <subcommand> [args...]\n\n");
    fprintf(out, "Subcommands:\n");
    for (size_t i = 0; i < g_cmds_count; i++) {
        fprintf(out, "  %-12s %s\n", g_cmds[i].name, g_cmds[i].summary);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_help(stderr);
        return 1;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(stdout);
        return 0;
    }
    for (size_t i = 0; i < g_cmds_count; i++) {
        if (strcmp(argv[1], g_cmds[i].name) == 0) {
            return g_cmds[i].fn(argc - 1, argv + 1);
        }
    }
    fprintf(stderr, "ffnet: unknown subcommand: %s\n\n", argv[1]);
    print_help(stderr);
    return 1;
}
