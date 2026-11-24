#!/usr/bin/env bash
# tests/lint.sh - project rules that the compiler does not check (issue #21).
#
#   tests/lint.sh [ROOT]       scan ROOT (default: repository root); prints every
#                              violation as file:line and exits 1 if there is any
#   tests/lint.sh --self-test  build a violation for every rule in a temp dir and
#                              check that the scan catches it (guards against a
#                              regex that silently matches nothing)
#
# Comments (block, also over several lines, and //) are ignored, and so are the
# contents of string literals (except for the arrow-format rule). One line can
# be waived with  /* lint:ok reason */ .
# Rules apply to src/ and include/ (production code) unless the table says
# otherwise; only the unsafe-libc rule also covers tests/.
set -u

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
DEFAULT_ROOT="$(cd "$(dirname "$SELF")/.." && pwd)"

# id <TAB> scope (space separated, relative to ROOT) <TAB> exempt basenames
#    <TAB> mode <TAB> ERE <TAB> message          ("-" = none; mode "str" keeps
# string literals, mode "code" blanks them; comments are always dropped)
# Only bracket expressions in the ERE: awk -v would eat backslashes.
B='(^|[^A-Za-z0-9_])'
RULES=()
rule() { RULES+=("$1"$'\t'"$2"$'\t'"$3"$'\t'"$4"$'\t'"$5"$'\t'"$6"); }

rule no-alloc      "src"     "-" code "${B}(malloc|calloc|realloc|free|strdup|asprintf|alloca)[ ]*[(]" \
     "dynamic allocation (spec 11: static tables only)"
rule no-printf     "src include" "util.c cli.c main.c" code \
     "${B}(printf|fprintf|puts|putchar|perror|dprintf)[ ]*[(]" \
     "direct output; use LOG_* (D9). Allowed only in util.c, cli.c, main.c"
rule no-realtime   "src include" "util.c node.c" code \
     "CLOCK_REALTIME|gettimeofday|${B}time[ ]*[(]" \
     "wall clock; logic runs on CLOCK_MONOTONIC only (spec 15)"
rule time-param    "src/membership.c src/failure.c src/proto.c src/net.c" "-" code \
     "${B}(now_ms|time)[ ]*[(]" \
     "this module takes time as a parameter (tests must not sleep)"
rule no-blocking   "src"     "-" code "${B}(signal|sleep|usleep|nanosleep)[ ]*[(]" \
     "use sigaction and poll timeouts, never sleep or signal() (#6)"
rule no-reuse      "src"     "-" code "SO_REUSEADDR|SO_REUSEPORT" \
     "two processes must not share a port (D8)"
rule proto-bytes   "src/proto.c include/proto.h" "-" code \
     "${B}(packed|htons|htonl|ntohs|ntohl)([^A-Za-z0-9_]|$)|#[ ]*pragma[ ]+pack" \
     "wire format is assembled byte by byte (N5, #4)"
rule no-exit       "src"     "main.c" code "${B}exit[ ]*[(]" \
     "exit() only in main.c (#12)"
rule no-abort      "src"     "util.c" code "${B}abort[ ]*[(]" \
     "abort() only in util.c (#2)"
rule no-rand       "src"     "-" code "${B}(rand|srand)[ ]*[(]" \
     "one RNG for the whole program: use rng_* from util (#2)"
rule arrow-format  "src include" "membership.c" str '"[^"]* -> [^"]*"' \
     "state transitions are logged only by mem_log_transition (D18)"
rule unsafe-libc   "src include tests" "-" code "${B}(strcpy|sprintf|gets)[ ]*[(]" \
     "unbounded libc call; use snprintf and explicit lengths"

list_files() { # root scope...
    local root=$1 p
    shift
    for p in "$@"; do
        if [ -d "$root/$p" ]; then
            find "$root/$p" -type f \( -name '*.c' -o -name '*.h' \)
        elif [ -f "$root/$p" ]; then
            echo "$root/$p"
        fi
    done | sort -u
}

scan_file() { # file shown-name id mode regex message
    # strip() drops block and // comments (also across lines) and, in "code"
    # mode, the contents of string and char literals, so only real code is
    # matched. "str" mode keeps literals (the arrow-format rule needs them).
    # \047 is a single quote (cannot appear inside the shell quoting).
    awk -v file="$2" -v id="$3" -v mode="$4" -v re="$5" -v msg="$6" '
        function strip(s, keep,    out, i, n, c, d, q) {
            out = ""; n = length(s); i = 1
            while (i <= n) {
                c = substr(s, i, 1); d = substr(s, i, 2)
                if (inblk) {
                    if (d == "*/") { inblk = 0; i += 2 } else i++
                    continue
                }
                if (d == "/*") { inblk = 1; i += 2; continue }
                if (d == "//") break
                if (c == "\"" || c == "\047") {
                    q = c; out = out c; i++
                    while (i <= n) {
                        c = substr(s, i, 1)
                        if (c == "\\") { if (keep) out = out substr(s, i, 2); i += 2; continue }
                        if (c == q) { out = out c; i++; break }
                        if (keep) out = out c
                        i++
                    }
                    continue
                }
                out = out c; i++
            }
            return out
        }
        { raw = $0; sub(/\r$/, "", raw); code = strip(raw, mode == "str") }
        raw ~ /lint:ok/ { next }
        code ~ re { printf "%s:%d: [%s] %s\n", file, NR, id, msg; bad = 1 }
        END { exit bad }
    ' "$1"
}

run_lint() { # root
    local root=$1 status=0 r id scope exempt mode re msg f base
    for r in "${RULES[@]}"; do
        IFS=$'\t' read -r id scope exempt mode re msg <<<"$r"
        # shellcheck disable=SC2086
        while IFS= read -r f; do
            base=$(basename "$f")
            case " $exempt " in *" $base "*) continue ;; esac
            scan_file "$f" "${f#"$root"/}" "$id" "$mode" "$re" "$msg" || status=1
        done < <(list_files "$root" $scope)
    done

    # Informational, never fails: every memcpy needs a bounds check next to it (spec 11).
    for f in $(list_files "$root" src/proto.c src/net.c src/cli.c); do
        awk -v file="${f#"$root"/}" '/memcpy/ { printf "info: %s:%d: %s\n", file, NR, $0 }' "$f"
    done

    return $status
}

# ---------------------------------------------------------------- self-test
selftest() {
    local bad=0 out rc
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT

    put() { mkdir -p "$tmp/$(dirname "$1")"; printf '%s\n' "$2" >"$tmp/$1"; }

    expect_fail() { # rule-id relpath code
        rm -rf "${tmp:?}"/*
        put "$2" "$3"
        out=$(bash "$SELF" "$tmp" 2>&1)
        rc=$?
        if [ $rc -ne 0 ] && grep -q "\[$1\]" <<<"$out"; then
            echo "ok    $1: $3"
        else
            echo "FAIL  $1 not caught: $3" >&2
            bad=1
        fi
    }

    expect_pass() { # relpath code
        rm -rf "${tmp:?}"/*
        put "$1" "$2"
        out=$(bash "$SELF" "$tmp" 2>&1)
        rc=$?
        if [ $rc -eq 0 ]; then
            echo "ok    clean: $2"
        else
            echo "FAIL  false positive: $2" >&2
            echo "$out" >&2
            bad=1
        fi
    }

    expect_fail no-alloc     src/a.c          'char *p = malloc(10);'
    expect_fail no-alloc     src/a.c          '    free(p);'
    expect_fail no-printf    src/node.c       'printf("x");'
    expect_fail no-printf    src/a.c          'fprintf(stderr, "x");'
    expect_fail no-realtime  src/a.c          'clock_gettime(CLOCK_REALTIME, &ts);'
    expect_fail no-realtime  src/a.c          't = time(NULL);'
    expect_fail time-param   src/membership.c 'uint64_t t = now_ms();'
    expect_fail time-param   src/failure.c    't = time(NULL);'
    expect_fail no-blocking  src/a.c          'sleep(1);'
    expect_fail no-blocking  src/a.c          'usleep(5);'
    expect_fail no-blocking  src/a.c          'signal(SIGINT, h);'
    expect_fail no-reuse     src/net.c        'setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);'
    expect_fail proto-bytes  src/proto.c      'x = htons(y);'
    expect_fail proto-bytes  src/proto.c      'struct __attribute__((packed)) hdr { int a; };'
    expect_fail proto-bytes  src/proto.c      '#pragma pack(1)'
    expect_fail no-exit      src/node.c       'exit(1);'
    expect_fail no-abort     src/a.c          'abort();'
    expect_fail no-rand      src/a.c          'x = rand() % 3;'
    expect_fail no-rand      src/a.c          'srand(1);'
    expect_fail arrow-format src/a.c          'LOG_I("member %u: A -> B", id);'
    expect_fail unsafe-libc  tests/t.c        'strcpy(a, b);'
    expect_fail unsafe-libc  src/a.c          'sprintf(buf, "%d", 1);'
    expect_fail unsafe-libc  include/a.h      'gets(buf);'

    expect_pass src/util.c       'printf("x"); abort(); t = time(NULL); clock_gettime(CLOCK_REALTIME, &ts);'
    expect_pass src/main.c       'exit(2); puts("usage");'
    expect_pass src/cli.c        'fprintf(out, "ok\n");'
    expect_pass src/node.c       't = time(NULL);'
    expect_pass src/membership.c 'LOG_I("member %u: %s -> %s", id, a, b);'
    expect_pass src/a.c          '// malloc(1) is forbidden here'
    expect_pass src/a.c          '/* free(p) */'
    expect_pass src/a.c          'x = 1; /* time(NULL) is not called */'
    expect_pass src/a.c          'x = 1; // rand() is not called'
    expect_pass src/a.c          'p = malloc(1); /* lint:ok fixture for the self-test */'
    expect_pass src/a.c          'snprintf(b, n, "x"); mem_free(p); fgets(b, n, f);'
    expect_pass src/a.c          'clock_gettime(CLOCK_MONOTONIC, &ts); n->count = n->count + 1;'
    expect_pass src/a.c          'uint64_t now_ms(void); void f(uint64_t now_ms);'
    expect_pass src/proto.c      'put_u16(buf, v); x = (v >> 8) & 0xFF;'
    expect_pass include/a.h      'const char *state_name(uint8_t s); /* "ALIVE", "?"; unknown -> "?" */'
    expect_pass src/a.c          'x = f("free(p) then sleep(1)"); c = 0x27; d = (char)39;'
    expect_pass src/a.c          $'/*\nfree(p);\n  sleep(1);\n*/\nx = 1;'
    expect_fail no-alloc src/a.c $'/* note\n*/ p = malloc(1);'
    expect_fail no-alloc src/a.c $'x = 1; /* a */ p = malloc(1);'

    if [ $bad -ne 0 ]; then
        echo "lint self-test: FAILED" >&2
        return 1
    fi
    echo "lint self-test: all rules fire, no false positives"
}

case "${1:-}" in
--self-test) selftest ;;
-h | --help) sed -n '2,15p' "$SELF" ;;
*)
    root="${1:-$DEFAULT_ROOT}"
    if run_lint "$root"; then
        echo "lint: OK"
    else
        echo "lint: FAILED" >&2
        exit 1
    fi
    ;;
esac
