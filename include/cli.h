/*
 * cli.h - user commands on stdin (F8, spec 8; decisions D9, D10, D13, D16).
 *
 * The output of these commands is a CONTRACT for the scripts (S1-S8, measurements,
 * demo): the format is fixed by D16 and must not change without a README change in
 * the same PR.  cli.c touches neither the socket nor the timers: it only reads
 * node_t and sets the flags frozen / leaving / running.
 */
#ifndef CLI_H
#define CLI_H

#include <stdint.h>
#include <stdio.h>

#include "common.h"

/* Longest accepted line (without '\n'); a longer one gets "ERR line too long". */
#define CLI_MAX_LINE 512

/*
 * Handles one line (without the trailing '\n') and writes the answer to `out`
 * (stdout, D9); every answer ends with fflush(out).  Commands:
 *   list     header, one row per member including self sorted by ID, `# total=...`
 *   stats    `key value` rows, then `# end`
 *   fail     OK frozen          (D10)
 *   recover  OK recovered
 *   leave    OK leaving         (orderly LEAVE, then exit; D13)
 *   quit     OK bye             (exit without LEAVE; D13)
 *   help     one line per command, then `# end`
 * An empty line is ignored; anything else gets `ERR ...`.  Every command is safe
 * in any state (`list` works while frozen, `fail` twice is not an error).
 */
void cli_handle_line(node_t *n, const char *line, FILE *out);

/* extension */
/* Same as cli_handle_line with an injected clock, so tests do not depend on now_ms(). */
void cli_exec(node_t *n, const char *line, FILE *out, uint64_t now_ms);

#endif /* CLI_H */
