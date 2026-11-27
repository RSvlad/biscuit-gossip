/* Minimal driver (--id, --bind, --verbose) until #12 implements full argument parsing. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "net.h"
#include "node.h"
#include "util.h"

static int parse_node_id(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return -1;
    }

    uint64_t v = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > UINT32_MAX) {
            return -1;
        }
    }
    if (v == 0) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr, "Usage: %s --id <n> --bind <ip:port> [--verbose]\n", prog);
}

int main(int argc, char **argv)
{
    static node_t node;
    config_t cfg;
    uint32_t id = 0;
    bool has_id = false;
    bool has_bind = false;
    const char *prog = (argc > 0 && argv[0] != NULL) ? argv[0] : "biscuitd";

    config_defaults(&cfg);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--id") == 0) {
            if (i + 1 >= argc || parse_node_id(argv[i + 1], &id) != 0) {
                fprintf(stderr, "error: invalid --id\n");
                print_usage(prog);
                return 2;
            }
            has_id = true;
            i++;
        } else if (strcmp(arg, "--bind") == 0) {
            if (i + 1 >= argc || net_parse_addr(argv[i + 1], &cfg.bind_addr) != 0) {
                fprintf(stderr, "error: invalid --bind (expected a.b.c.d:port)\n");
                print_usage(prog);
                return 2;
            }
            has_bind = true;
            i++;
        } else if (strcmp(arg, "--verbose") == 0) {
            cfg.verbose = true;
        } else if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            print_usage(prog);
            return 0;
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", arg);
            print_usage(prog);
            return 2;
        }
    }

    if (!has_id || !has_bind) {
        fprintf(stderr, "error: --id and --bind are required\n");
        print_usage(prog);
        return 2;
    }

    log_init(cfg.verbose ? LOG_DEBUG : LOG_INFO, id);

    if (node_init(&node, &cfg, id, ROLE_MIXER) != 0) {
        return 1;
    }

    node_run(&node);
    node_shutdown(&node);
    return node.exit_code; /* 0, or 3 when the seed never answered (D17, #9) */
}
