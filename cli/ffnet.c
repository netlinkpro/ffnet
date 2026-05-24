// SPDX-License-Identifier: LGPL-2.1-or-later
//
// Adding a new subcommand:
//   1. Create cli/cmd_<name>.c with `int cmd_<name>(int argc, char **argv)`
//      and a local `usage()` helper invoked on `--help` / `-h` / bad args.
//   2. Add an `extern` declaration below.
//   3. Add an entry to g_cmds[] with a one-line summary.
//   4. Add the .o to CLI_OBJ in the Makefile.
// Top-level `ffnet --help` auto-includes any new entry from g_cmds[].

#include <signal.h>
#include <stdio.h>
#include <string.h>

extern int cmd_ws_echo(int argc, char **argv);
extern int cmd_tcp_echo(int argc, char **argv);

typedef struct Subcommand {
    const char *name;
    int       (*fn)(int argc, char **argv);
    const char *summary;
} Subcommand;

static const Subcommand g_cmds[] = {
    { "ws-echo",  cmd_ws_echo,  "Run a WebSocket echo server" },
    { "tcp-echo", cmd_tcp_echo, "Run a raw TCP echo server (event loop demo)" },
};
static const size_t g_cmds_count = sizeof(g_cmds) / sizeof(g_cmds[0]);

static void print_help(FILE *out)
{
    fprintf(out, "ffnet — universal performant network library\n\n");
    fprintf(out, "usage:\n");
    fprintf(out, "  ffnet <subcommand> [args...]\n");
    fprintf(out, "  ffnet --help\n\n");
    fprintf(out, "subcommands:\n");
    for (size_t i = 0; i < g_cmds_count; i++) {
        fprintf(out, "  %-12s %s\n", g_cmds[i].name, g_cmds[i].summary);
    }
    fprintf(out, "\nRun `ffnet <subcommand> --help` for subcommand-specific help.\n");
}

int main(int argc, char **argv)
{
    /* Network servers must not die when a peer half-closes; let write()
     * return EPIPE/-1 instead so we can translate to FFE_PIPE. */
    signal(SIGPIPE, SIG_IGN);

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
