#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>

#define MAX_LINE 4096
#define MAX_NAME 256
#define MAX_ARGS 128
#define UMK_HASH_HEX 33

#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_RESET   "\033[0m"

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        perror("malloc");
        exit(1);
    }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n);
    if (!r) {
        perror("realloc");
        exit(1);
    }
    return r;
}

static char *xstrdup(const char *s) {
    char *r = strdup(s);
    if (!r) {
        perror("strdup");
        exit(1);
    }
    return r;
}

typedef struct {
    char **items;
    int count;
    int capacity;
} StrVec;

static void strvec_init(StrVec *v) {
    v->items = NULL;
    v->count = 0;
    v->capacity = 0;
}

static void strvec_add(StrVec *v, const char *str) {
    if (v->count >= v->capacity) {
        v->capacity = v->capacity ? v->capacity * 2 : 4;
        v->items = xrealloc(v->items, (size_t)v->capacity * sizeof(char *));
    }
    v->items[v->count++] = xstrdup(str);
}

static int strvec_contains(StrVec *v, const char *str) {
    for (int i = 0; i < v->count; i++) {
        if (strcmp(v->items[i], str) == 0) return 1;
    }
    return 0;
}

static void strvec_free(StrVec *v) {
    for (int i = 0; i < v->count; i++) free(v->items[i]);
    free(v->items);
    strvec_init(v);
}

static void strvec_clear(StrVec *v) {
    for (int i = 0; i < v->count; i++) free(v->items[i]);
    v->count = 0;
}

static void strvec_pop(StrVec *v) {
    if (v->count > 0) {
        free(v->items[v->count - 1]);
        v->count--;
    }
}

typedef struct Flag {
    char name[MAX_NAME];
    int type;
    StrVec commands;
    struct Flag *next;
} Flag;

typedef struct Rule {
    char *target;
    StrVec deps;
    StrVec commands;
    struct Rule *next;
} Rule;

typedef struct Command {
    char name[MAX_NAME];
    StrVec commands;
    Flag *flags;
    struct Command *next;
} Command;

typedef struct Variable {
    char name[MAX_NAME];
    char *value;
    struct Variable *next;
} Variable;

enum {
    JOB_RULE = 0,
    JOB_FILE = 1
};

typedef struct Job {
    char *target;
    Rule *rule;
    int kind;
    StrVec deps;
    StrVec pre_commands;
    StrVec final_cmds;
    int deps_remaining;
    int forced;
    int state;
    int queued;
    pid_t pid;
    struct Job *next;
} Job;

typedef struct CacheEntry {
    char *path;
    char hash[UMK_HASH_HEX];
    struct CacheEntry *next;
} CacheEntry;

typedef struct RuntimeHash {
    char *path;
    char hash[UMK_HASH_HEX];
    struct RuntimeHash *next;
} RuntimeHash;

typedef struct UmkCtx {
    Rule *rules;
    Command *commands;
    Variable *variables;
    CacheEntry *cache;
    RuntimeHash *rt_hashes;
    char **global_flags;
    int global_flag_count;
    int use_color;
    int dry_run;
    int parallel_jobs;
    int parallel_auto;
    int j_from_cmdline;
    Job *all_jobs;
    Job **ready_queue;
    int ready_queue_size;
    int ready_queue_capacity;
    int jobs_running;
    int build_failed;
    int interrupted;
    StrVec done_commands;
} UmkCtx;

static UmkCtx *active_ctx = NULL;

static void trim(char *str);
static int is_blank(const char *str);
static void expand(UmkCtx *ctx, const char *str, char *out, size_t out_size);
static void add_variable(UmkCtx *ctx, const char *name, const char *value);
static char *get_variable(UmkCtx *ctx, const char *name);
static void parse_umkfile(UmkCtx *ctx, const char *filename);
static int execute_target(UmkCtx *ctx, const char *target, int parallel_allowed);
static int execute_command_by_name(UmkCtx *ctx, const char *name, int parallel_allowed);
static int execute_serial_rule(UmkCtx *ctx, const char *target);
static int execute_parallel_rule(UmkCtx *ctx, const char *target);
static int execute_shell_safe(UmkCtx *ctx, const char *cmd_line);
static int needs_rebuild(UmkCtx *ctx, const char *target, StrVec *deps);
static void mark_rebuilt(UmkCtx *ctx, const char *target, StrVec *deps);
static int match_pattern(const char *name, const char *pattern);
static int apply_pattern(const char *target, const char *pattern, char *out, size_t out_size);
static void wildcard(const char *pattern, char *out, size_t out_size);
static void shell_cmd(const char *cmd, char *out, size_t out_size);
static void print_color(UmkCtx *ctx, const char *color, const char *msg);
static void free_all(UmkCtx *ctx);
static int exec_command_list(UmkCtx *ctx, StrVec *cmds, int parallel_allowed);
static int exec_flags_of_type(UmkCtx *ctx, Command *c, int type, int parallel_allowed);
static Flag *parse_flag_line(const char *line);
static int get_cpu_count(void);
static Command *find_command(UmkCtx *ctx, const char *name);
static Rule *find_rule(UmkCtx *ctx, const char *name);
static void resolve_rule_deps(Rule *r, const char *target, StrVec *out);
static void add_ready_job(UmkCtx *ctx, Job *job);
static Job *pop_ready_job(UmkCtx *ctx);
static Job *find_job_by_pid(UmkCtx *ctx, pid_t pid);
static Job *find_job_by_target(UmkCtx *ctx, const char *target);
static void mark_dependency_done(UmkCtx *ctx, Job *job);
static void free_job_graph(UmkCtx *ctx);
static Job *build_rule_graph(UmkCtx *ctx, const char *target, StrVec *path, int *err);
static int job_prepare_commands(UmkCtx *ctx, Job *job);
static void handle_sigint(int sig);
static void expand_autovars(const char *cmd, const char *target, StrVec *deps, char *out, size_t out_size);

typedef struct UmkHash {
    uint64_t h0;
    uint64_t h1;
    uint64_t len;
    unsigned char buf[16];
    size_t buflen;
} UmkHash;

static uint64_t rotl64(uint64_t x, int r) {
    return (x << r) | (x >> (64 - r));
}

static uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

static uint64_t read_le64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i]) << (8 * i);
    }
    return v;
}

static void umk_hash_block(UmkHash *h, const unsigned char *p) {
    uint64_t x0 = read_le64(p);
    uint64_t x1 = read_le64(p + 8);

    h->h0 ^= x0;
    h->h0 = rotl64(h->h0, 31);
    h->h0 *= 0x9E3779B185EBCA87ULL;

    h->h1 ^= x1;
    h->h1 = rotl64(h->h1, 29);
    h->h1 *= 0xC2B2AE3D27D4EB4FULL;

    uint64_t t = h->h0 + h->h1;
    h->h0 ^= rotl64(t, 17);
    h->h1 ^= rotl64(t, 37);

    h->h0 += h->h1 * 0x165667B19E3779F9ULL;
    h->h1 += h->h0 * 0x85EBCA77C2B2AE63ULL;
}

static void umk_hash_init(UmkHash *h) {
    h->h0 = 0x9E3779B185EBCA87ULL;
    h->h1 = 0xC2B2AE3D27D4EB4FULL;
    h->len = 0;
    h->buflen = 0;
}

static void umk_hash_update(UmkHash *h, const void *data, size_t n) {
    const unsigned char *p = data;
    h->len += (uint64_t)n;

    if (h->buflen > 0) {
        while (n > 0 && h->buflen < 16) {
            h->buf[h->buflen++] = *p++;
            n--;
        }
        if (h->buflen == 16) {
            umk_hash_block(h, h->buf);
            h->buflen = 0;
        }
    }

    while (n >= 16) {
        umk_hash_block(h, p);
        p += 16;
        n -= 16;
    }

    if (n > 0) {
        memcpy(h->buf, p, n);
        h->buflen = n;
    }
}

static void umk_hash_final(UmkHash *h, char out[UMK_HASH_HEX]) {
    unsigned char pad[16];
    memset(pad, 0, sizeof(pad));
    if (h->buflen > 0) memcpy(pad, h->buf, h->buflen);
    pad[h->buflen] = 0x80;
    umk_hash_block(h, pad);

    unsigned char lenblk[16];
    memset(lenblk, 0, sizeof(lenblk));
    for (int i = 0; i < 8; i++) {
        lenblk[i] = (unsigned char)((h->len >> (8 * i)) & 0xFF);
    }
    lenblk[8] = 0xA5;
    umk_hash_block(h, lenblk);

    h->h0 ^= h->len;
    h->h1 ^= h->len * 0x27D4EB2F165667C5ULL;

    h->h0 = fmix64(h->h0);
    h->h1 = fmix64(h->h1);
    h->h0 ^= h->h1;
    h->h1 ^= h->h0;

    unsigned char raw[16];
    for (int i = 0; i < 8; i++) {
        raw[i] = (unsigned char)((h->h0 >> (8 * i)) & 0xFF);
        raw[8 + i] = (unsigned char)((h->h1 >> (8 * i)) & 0xFF);
    }

    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02x", raw[i]);
    }
    out[32] = 0;
}

static int umk_hash_file(const char *path, char out[UMK_HASH_HEX]) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    UmkHash h;
    umk_hash_init(&h);

    unsigned char buf[65536];
    size_t bytes;
    int ok = 1;

    while ((bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        umk_hash_update(&h, buf, bytes);
    }

    if (ferror(f)) ok = 0;
    fclose(f);
    if (!ok) return -1;

    umk_hash_final(&h, out);
    return 0;
}

static void load_cache(UmkCtx *ctx) {
    ctx->cache = NULL;
    FILE *f = fopen(".umk_cache", "r");
    if (!f) return;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (is_blank(line)) continue;
        if (strlen(line) < 34 || line[32] != ' ') continue;

        char hash[UMK_HASH_HEX];
        memcpy(hash, line, 32);
        hash[32] = 0;

        const char *path = line + 33;
        if (is_blank(path)) continue;

        CacheEntry *entry = xmalloc(sizeof(CacheEntry));
        entry->path = xstrdup(path);
        memcpy(entry->hash, hash, UMK_HASH_HEX);
        entry->next = ctx->cache;
        ctx->cache = entry;
    }

    fclose(f);
}

static void save_cache(UmkCtx *ctx) {
    FILE *f = fopen(".umk_cache", "w");
    if (!f) return;

    CacheEntry *curr = ctx->cache;
    while (curr) {
        fprintf(f, "%s %s\n", curr->hash, curr->path);
        curr = curr->next;
    }

    fclose(f);
}

static const char *get_cached_hash(UmkCtx *ctx, const char *path) {
    for (CacheEntry *curr = ctx->cache; curr; curr = curr->next) {
        if (strcmp(curr->path, path) == 0) return curr->hash;
    }
    return NULL;
}

static void set_runtime_hash(UmkCtx *ctx, const char *path, const char *hash) {
    for (RuntimeHash *r = ctx->rt_hashes; r; r = r->next) {
        if (strcmp(r->path, path) == 0) {
            memcpy(r->hash, hash, UMK_HASH_HEX);
            return;
        }
    }

    RuntimeHash *nr = xmalloc(sizeof(RuntimeHash));
    nr->path = xstrdup(path);
    memcpy(nr->hash, hash, UMK_HASH_HEX);
    nr->next = ctx->rt_hashes;
    ctx->rt_hashes = nr;
}

static void set_cached_hash(UmkCtx *ctx, const char *path, const char *hash) {
    for (CacheEntry *curr = ctx->cache; curr; curr = curr->next) {
        if (strcmp(curr->path, path) == 0) {
            memcpy(curr->hash, hash, UMK_HASH_HEX);
            set_runtime_hash(ctx, path, hash);
            return;
        }
    }

    CacheEntry *entry = xmalloc(sizeof(CacheEntry));
    entry->path = xstrdup(path);
    memcpy(entry->hash, hash, UMK_HASH_HEX);
    entry->next = ctx->cache;
    ctx->cache = entry;
    set_runtime_hash(ctx, path, hash);
}

static int get_current_hash(UmkCtx *ctx, const char *path, char out[UMK_HASH_HEX]) {
    for (RuntimeHash *r = ctx->rt_hashes; r; r = r->next) {
        if (strcmp(r->path, path) == 0) {
            memcpy(out, r->hash, UMK_HASH_HEX);
            return 0;
        }
    }

    if (umk_hash_file(path, out) != 0) return -1;

    RuntimeHash *nr = xmalloc(sizeof(RuntimeHash));
    nr->path = xstrdup(path);
    memcpy(nr->hash, out, UMK_HASH_HEX);
    nr->next = ctx->rt_hashes;
    ctx->rt_hashes = nr;

    return 0;
}

static int needs_rebuild(UmkCtx *ctx, const char *target, StrVec *deps) {
    char cur[UMK_HASH_HEX];

    if (access(target, F_OK) != 0) return 1;
    if (get_current_hash(ctx, target, cur) != 0) return 1;

    const char *cached = get_cached_hash(ctx, target);
    if (!cached || strcmp(cur, cached) != 0) return 1;

    for (int i = 0; i < deps->count; i++) {
        const char *dep = deps->items[i];

        if (access(dep, F_OK) != 0) return 1;
        if (get_current_hash(ctx, dep, cur) != 0) return 1;

        cached = get_cached_hash(ctx, dep);
        if (!cached || strcmp(cur, cached) != 0) return 1;
    }

    return 0;
}

static void mark_rebuilt(UmkCtx *ctx, const char *target, StrVec *deps) {
    char h[UMK_HASH_HEX];

    if (umk_hash_file(target, h) == 0) {
        set_cached_hash(ctx, target, h);
    }

    for (int i = 0; i < deps->count; i++) {
        if (umk_hash_file(deps->items[i], h) == 0) {
            set_cached_hash(ctx, deps->items[i], h);
        }
    }
}

static void trim(char *str) {
    char *s = str;
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) {
        str[0] = 0;
        return;
    }

    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) e--;

    memmove(str, s, (size_t)(e - s + 1));
    str[e - s + 1] = 0;
}

static int is_blank(const char *str) {
    while (*str) {
        if (!isspace((unsigned char)*str++)) return 0;
    }
    return 1;
}

static void print_color(UmkCtx *ctx, const char *color, const char *msg) {
    if (ctx->use_color && isatty(STDERR_FILENO)) {
        fprintf(stderr, "%s%s%s\n", color, msg, COLOR_RESET);
    } else {
        fprintf(stderr, "%s\n", msg);
    }
}

static void add_variable(UmkCtx *ctx, const char *name, const char *value) {
    for (Variable *v = ctx->variables; v; v = v->next) {
        if (strcmp(v->name, name) == 0) {
            free(v->value);
            v->value = xstrdup(value);
            return;
        }
    }

    Variable *nv = xmalloc(sizeof(Variable));
    strncpy(nv->name, name, MAX_NAME - 1);
    nv->name[MAX_NAME - 1] = 0;
    nv->value = xstrdup(value);
    nv->next = ctx->variables;
    ctx->variables = nv;
}

static char *get_variable(UmkCtx *ctx, const char *name) {
    for (Variable *v = ctx->variables; v; v = v->next) {
        if (strcmp(v->name, name) == 0) return v->value;
    }
    return NULL;
}

static int match_pattern_helper(const char *name, const char *pattern) {
    const char *pp = pattern;
    const char *nn = name;

    while (*pp) {
        char p_char = *pp;
        if (p_char == '%') p_char = '*';

        if (p_char == '*') {
            pp++;
            if (!*pp) return 1;
            while (*nn) {
                if (match_pattern_helper(nn, pp)) return 1;
                nn++;
            }
            return 0;
        } else if (p_char != *nn) {
            return 0;
        }

        pp++;
        nn++;
    }

    return *nn == 0;
}

static int match_pattern(const char *name, const char *pattern) {
    return match_pattern_helper(name, pattern);
}

static int apply_pattern(const char *target, const char *pattern, char *out, size_t out_size) {
    out[0] = 0;

    char p[MAX_LINE];
    if (strlen(pattern) >= MAX_LINE) return 0;

    strcpy(p, pattern);
    for (char *x = p; *x; x++) {
        if (*x == '%') *x = '*';
    }

    char *star = strchr(p, '*');
    if (!star) {
        strncpy(out, pattern, out_size - 1);
        out[out_size - 1] = 0;
        return 1;
    }

    int pre = (int)(star - p);
    int suf = (int)strlen(p) - (pre + 1);

    if (strncmp(target, p, (size_t)pre) != 0) return 0;

    int tlen = (int)strlen(target);
    if (tlen < pre + suf) return 0;

    if (suf > 0 && strcmp(target + tlen - suf, p + pre + 1) != 0) return 0;

    int stem_len = tlen - pre - suf;
    if ((size_t)stem_len >= out_size) stem_len = (int)(out_size - 1);

    strncpy(out, target + pre, (size_t)stem_len);
    out[stem_len] = 0;
    return 1;
}

static void wildcard(const char *pattern, char *out, size_t out_size) {
    out[0] = 0;

    DIR *d = opendir(".");
    if (!d) return;

    struct dirent *e;
    size_t len = 0;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;

        if (match_pattern(e->d_name, pattern)) {
            size_t nlen = strlen(e->d_name);
            if (len + nlen + 2 >= out_size) break;

            if (len > 0) out[len++] = ' ';
            strcpy(out + len, e->d_name);
            len += nlen;
            out[len] = 0;
        }
    }

    closedir(d);
}

static void shell_cmd(const char *cmd, char *out, size_t out_size) {
    out[0] = 0;

    FILE *f = popen(cmd, "r");
    if (!f) return;

    char line[MAX_LINE];
    if (fgets(line, sizeof(line), f)) {
        trim(line);
        size_t len = strlen(line);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, line, len);
        out[len] = 0;
    }

    pclose(f);
}

static void expand(UmkCtx *ctx, const char *str, char *out, size_t out_size) {
    size_t len = 0;
    out[0] = 0;

    const char *p = str;
    while (*p && len < out_size - 1) {
        if (*p == '$' && p[1] == '$') {
            out[len++] = '$';
            p += 2;
            out[len] = 0;
            continue;
        }

        if (*p == '$' && p[1] == '(') {
            const char *end = strchr(p, ')');
            if (!end) break;

            char inner[1024];
            size_t ilen = (size_t)(end - p - 2);
            if (ilen >= sizeof(inner)) ilen = sizeof(inner) - 1;

            strncpy(inner, p + 2, ilen);
            inner[ilen] = 0;

            char *space = strchr(inner, ' ');
            char *subst = strchr(inner, ':');

            if (space && (!subst || space < subst)) {
                *space = 0;
                char *args = space + 1;

                if (strcmp(inner, "wildcard") == 0) {
                    char w_buf[MAX_LINE];
                    wildcard(args, w_buf, sizeof(w_buf));

                    size_t wlen = strlen(w_buf);
                    if (len + wlen >= out_size) wlen = out_size - len - 1;

                    memcpy(out + len, w_buf, wlen);
                    len += wlen;
                    out[len] = 0;
                } else if (strcmp(inner, "shell") == 0) {
                    char s_buf[MAX_LINE];
                    shell_cmd(args, s_buf, sizeof(s_buf));

                    size_t slen = strlen(s_buf);
                    if (len + slen >= out_size) slen = out_size - len - 1;

                    memcpy(out + len, s_buf, slen);
                    len += slen;
                    out[len] = 0;
                }
            } else if (subst) {
                *subst = 0;
                char *var = inner;
                char *rule = subst + 1;
                char *eq = strchr(rule, '=');

                if (eq) {
                    *eq = 0;
                    char *from = rule;
                    char *to = eq + 1;
                    char *val = get_variable(ctx, var);

                    if (val) {
                        char temp_out[MAX_LINE];
                        temp_out[0] = 0;
                        size_t tlen = 0;

                        char *saveptr;
                        char *tok = strtok_r(val, " ", &saveptr);
                        while (tok) {
                            size_t t_len = strlen(tok);
                            size_t f_len = strlen(from);
                            int matches = 0;

                            if (f_len == 0) {
                                matches = 1;
                            } else if (t_len >= f_len && strcmp(tok + t_len - f_len, from) == 0) {
                                matches = 1;
                            }

                            if (matches) {
                                size_t prefix_len = t_len - f_len;
                                if (tlen + prefix_len + strlen(to) + 1 < sizeof(temp_out)) {
                                    memcpy(temp_out + tlen, tok, prefix_len);
                                    tlen += prefix_len;
                                    strcpy(temp_out + tlen, to);
                                    tlen += strlen(to);
                                    temp_out[tlen++] = ' ';
                                    temp_out[tlen] = 0;
                                }
                            } else {
                                if (tlen + t_len + 1 < sizeof(temp_out)) {
                                    memcpy(temp_out + tlen, tok, t_len);
                                    tlen += t_len;
                                    temp_out[tlen++] = ' ';
                                    temp_out[tlen] = 0;
                                }
                            }

                            tok = strtok_r(NULL, " ", &saveptr);
                        }

                        if (tlen > 0) temp_out[tlen - 1] = 0;

                        size_t t_out_len = strlen(temp_out);
                        if (len + t_out_len >= out_size) t_out_len = out_size - len - 1;

                        memcpy(out + len, temp_out, t_out_len);
                        len += t_out_len;
                        out[len] = 0;
                    }
                }
            } else {
                char *val = get_variable(ctx, inner);
                if (val) {
                    size_t vlen = strlen(val);
                    if (len + vlen >= out_size) vlen = out_size - len - 1;

                    memcpy(out + len, val, vlen);
                    len += vlen;
                    out[len] = 0;
                }
            }

            p = end + 1;
        } else {
            out[len++] = *p++;
            out[len] = 0;
        }
    }
}

static void expand_autovars(const char *cmd, const char *target, StrVec *deps, char *out, size_t out_size) {
    size_t len = 0;
    out[0] = 0;

    const char *p = cmd;
    while (*p && len < out_size - 1) {
        if (*p == '$') {
            if (p[1] == '@') {
                size_t tlen = strlen(target);
                if (len + tlen >= out_size) tlen = out_size - len - 1;

                memcpy(out + len, target, tlen);
                len += tlen;
                out[len] = 0;
                p += 2;
                continue;
            } else if (p[1] == '<') {
                if (deps && deps->count > 0) {
                    size_t dlen = strlen(deps->items[0]);
                    if (len + dlen >= out_size) dlen = out_size - len - 1;

                    memcpy(out + len, deps->items[0], dlen);
                    len += dlen;
                    out[len] = 0;
                }
                p += 2;
                continue;
            } else if (p[1] == '^') {
                if (deps) {
                    for (int i = 0; i < deps->count; i++) {
                        if (i > 0) {
                            if (len < out_size - 1) out[len++] = ' ';
                            out[len] = 0;
                        }

                        size_t dlen = strlen(deps->items[i]);
                        if (len + dlen >= out_size) dlen = out_size - len - 1;

                        memcpy(out + len, deps->items[i], dlen);
                        len += dlen;
                        out[len] = 0;
                    }
                }
                p += 2;
                continue;
            }
        }

        out[len++] = *p++;
        out[len] = 0;
    }
}

static Flag *parse_flag_line(const char *line) {
    char copy[MAX_LINE];
    if (strlen(line) >= MAX_LINE) return NULL;

    strcpy(copy, line);
    trim(copy);

    if (strncmp(copy, "-fg(", 4) != 0 && strncmp(copy, "+fg(", 4) != 0) {
        return NULL;
    }

    Flag *f = xmalloc(sizeof(Flag));
    strvec_init(&f->commands);
    f->next = NULL;
    f->type = (copy[0] == '-') ? 0 : 1;

    char *start = strchr(copy, '(');
    if (!start) {
        free(f);
        return NULL;
    }
    start++;

    char *end = strchr(start, ')');
    if (!end) {
        strvec_free(&f->commands);
        free(f);
        return NULL;
    }

    int len = (int)(end - start);
    if (len >= MAX_NAME) len = MAX_NAME - 1;

    strncpy(f->name, start, (size_t)len);
    f->name[len] = 0;

    return f;
}

static void parse_umkfile(UmkCtx *ctx, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        print_color(ctx, COLOR_RED, "No UMK file");
        exit(1);
    }

    char line[MAX_LINE];
    Command *cur_cmd = NULL;
    Flag *cur_flag = NULL;
    int in_flags = 0;
    int line_num = 0;

    int skip_depth = 0;
    int condition_stack[16];
    int cond_ptr = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        char orig[MAX_LINE];
        strcpy(orig, line);
        trim(line);

        if (is_blank(line)) continue;
        if (line[0] == '#') continue;

        if (strncmp(line, "if ", 3) == 0) {
            if (cond_ptr + 1 >= 16) {
                fprintf(stderr, "%s:%d: error: too many nested conditionals\n", filename, line_num);
                exit(1);
            }

            cond_ptr++;

            if (skip_depth > 0) {
                skip_depth++;
                continue;
            }

            char exp_line[MAX_LINE];
            expand(ctx, line + 3, exp_line, sizeof(exp_line));

            char *eq = strstr(exp_line, "==");
            int result = 0;

            if (eq) {
                *eq = 0;
                char *left = exp_line;
                char *right = eq + 2;
                trim(left);
                trim(right);
                if (strcmp(left, right) == 0) result = 1;
            } else {
                if (strlen(exp_line) > 0) result = 1;
            }

            condition_stack[cond_ptr] = result;
            if (!result) skip_depth = cond_ptr;
            continue;
        }

        if (strcmp(line, "else") == 0) {
            if (cond_ptr > 0) {
                if (skip_depth == cond_ptr) {
                    skip_depth = 0;
                    condition_stack[cond_ptr] = 1;
                } else if (skip_depth == 0 && condition_stack[cond_ptr] == 1) {
                    skip_depth = cond_ptr;
                    condition_stack[cond_ptr] = 0;
                }
            }
            continue;
        }

        if (strcmp(line, "endif") == 0) {
            if (cond_ptr > 0) {
                if (skip_depth == cond_ptr) skip_depth = 0;
                cond_ptr--;
            }
            continue;
        }

        if (skip_depth > 0) continue;

        if (strncmp(line, "threadreap", 10) == 0) {
            char *p = line + 10;
            while (isspace((unsigned char)*p)) p++;

            if (strcmp(p, "-auto") == 0) {
                ctx->parallel_auto = 1;
            } else if (*p == '-') {
                int n = atoi(p + 1);
                if (n > 0) ctx->parallel_jobs = n;
            }
            continue;
        }

        char *eq = strchr(line, '=');
        char *colon = strchr(line, ':');

        if (eq && !in_flags && !cur_cmd && (!colon || eq < colon)) {
            int is_append = 0;

            if (eq > line && eq[-1] == '+') {
                is_append = 1;
                eq[-1] = 0;
            }

            *eq = 0;
            char *name = line;
            char *val = eq + 1;

            trim(name);
            trim(val);

            char exp_val[MAX_LINE];
            expand(ctx, val, exp_val, sizeof(exp_val));

            if (is_append) {
                char *old = get_variable(ctx, name);
                char new_val[MAX_LINE * 2];

                if (old) {
                    snprintf(new_val, sizeof(new_val), "%s %s", old, exp_val);
                } else {
                    snprintf(new_val, sizeof(new_val), "%s", exp_val);
                }

                add_variable(ctx, name, new_val);
            } else {
                add_variable(ctx, name, exp_val);
            }

            continue;
        }

        if (colon && !in_flags && !cur_cmd) {
            *colon = 0;
            char *target = line;
            char *deps = colon + 1;

            trim(target);
            trim(deps);

            if (strlen(deps) == 0) {
                Command *cmd = xmalloc(sizeof(Command));
                strncpy(cmd->name, target, MAX_NAME - 1);
                cmd->name[MAX_NAME - 1] = 0;
                strvec_init(&cmd->commands);
                cmd->flags = NULL;
                cmd->next = ctx->commands;
                ctx->commands = cmd;
                cur_cmd = cmd;
            } else {
                Rule *r = xmalloc(sizeof(Rule));
                r->target = xstrdup(target);
                strvec_init(&r->deps);
                strvec_init(&r->commands);

                char exp_deps[MAX_LINE];
                expand(ctx, deps, exp_deps, sizeof(exp_deps));

                char *copy = xstrdup(exp_deps);
                char *tok = strtok(copy, " ");
                while (tok) {
                    strvec_add(&r->deps, tok);
                    tok = strtok(NULL, " ");
                }
                free(copy);

                int found_eoc = 0;

                while (fgets(line, sizeof(line), fp)) {
                    line_num++;

                    char trimmed[MAX_LINE];
                    strcpy(trimmed, line);
                    trim(trimmed);

                    if (is_blank(trimmed)) continue;
                    if (trimmed[0] == '#') continue;

                    if (strcmp(trimmed, "eoc") == 0) {
                        found_eoc = 1;
                        break;
                    }

                    if (isspace((unsigned char)line[0])) {
                        strvec_add(&r->commands, trimmed);
                    } else {
                        fprintf(stderr, "%s:%d: error: rule command must be indented (tab or spaces)\n", filename, line_num);
                        exit(1);
                    }
                }

                if (!found_eoc) {
                    fprintf(stderr, "%s:%d: error: missing eoc for rule '%s'\n", filename, line_num, r->target);
                    exit(1);
                }

                r->next = ctx->rules;
                ctx->rules = r;
            }

            continue;
        }

        if (strcmp(line, "eoc") == 0) {
            cur_cmd = NULL;
            in_flags = 0;
            cur_flag = NULL;
            continue;
        }

        if (!cur_cmd) {
            fprintf(stderr, "%s:%d: error: statement outside of any command/rule block: '%s'\n", filename, line_num, line);
            exit(1);
        }

        if (strcmp(line, "+flags:") == 0) {
            in_flags = 1;
            continue;
        }

        if (in_flags) {
            if (strcmp(line, ";") == 0) {
                in_flags = 0;
                cur_flag = NULL;
                continue;
            }

            Flag *new_flag = parse_flag_line(orig);
            if (new_flag) {
                new_flag->next = cur_cmd->flags;
                cur_cmd->flags = new_flag;
                cur_flag = new_flag;
                continue;
            }

            if (strcmp(line, "eofg") == 0) {
                cur_flag = NULL;
                continue;
            }

            if (cur_flag) {
                strvec_add(&cur_flag->commands, line);
            } else {
                fprintf(stderr, "%s:%d: error: flag commands defined without active flag group\n", filename, line_num);
                exit(1);
            }

            continue;
        }

        strvec_add(&cur_cmd->commands, line);
    }

    if (cond_ptr != 0) {
        fprintf(stderr, "%s:%d: error: unexpected end of file (unclosed conditional block)\n", filename, line_num);
        exit(1);
    }

    if (in_flags || cur_cmd) {
        fprintf(stderr, "%s:%d: error: unexpected end of file (unclosed command or flag block)\n", filename, line_num);
        exit(1);
    }

    fclose(fp);
}

static int execute_shell_safe(UmkCtx *ctx, const char *cmd_line) {
    if (ctx->dry_run) {
        printf("%s\n", cmd_line);
        return 0;
    }

    if (is_blank(cmd_line)) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        execlp("sh", "sh", "-c", cmd_line, (char *)NULL);
        execl("/bin/sh", "sh", "-c", cmd_line, (char *)NULL);
        perror("execl");
        _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return 1;
    }

    perror("fork");
    return 1;
}

static Command *find_command(UmkCtx *ctx, const char *name) {
    for (Command *c = ctx->commands; c; c = c->next) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static Rule *find_rule(UmkCtx *ctx, const char *name) {
    for (Rule *r = ctx->rules; r; r = r->next) {
        if (match_pattern(name, r->target)) return r;
    }
    return NULL;
}

static void substitute_stem(const char *pattern, const char *stem, char *out, size_t out_size) {
    size_t pos = 0;
    out[0] = 0;

    for (const char *p = pattern; *p && pos < out_size - 1; p++) {
        if (*p == '%') {
            size_t slen = strlen(stem);
            if (pos + slen >= out_size) slen = out_size - pos - 1;
            memcpy(out + pos, stem, slen);
            pos += slen;
        } else {
            out[pos++] = *p;
        }
    }

    out[pos] = 0;
}

static void resolve_rule_deps(Rule *r, const char *target, StrVec *out) {
    char stem[MAX_LINE] = "";
    int has_stem = 0;

    if (strchr(r->target, '%')) {
        has_stem = apply_pattern(target, r->target, stem, sizeof(stem));
    }

    for (int i = 0; i < r->deps.count; i++) {
        char buf[MAX_LINE];

        if (has_stem) {
            substitute_stem(r->deps.items[i], stem, buf, sizeof(buf));
        } else {
            strncpy(buf, r->deps.items[i], sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
        }

        if (!strvec_contains(out, buf)) {
            strvec_add(out, buf);
        }
    }
}

static int exec_flags_of_type(UmkCtx *ctx, Command *c, int type, int parallel_allowed) {
    for (Flag *f = c->flags; f; f = f->next) {
        if (f->type != type) continue;

        for (int i = 0; i < ctx->global_flag_count; i++) {
            char *flag_name = ctx->global_flags[i];

            if (flag_name[0] == '-') {
                flag_name += (flag_name[1] == '-') ? 2 : 1;
            }

            if (strcmp(f->name, flag_name) == 0) {
                int ret = exec_command_list(ctx, &f->commands, parallel_allowed);
                if (ret != 0) return ret;
                break;
            }
        }
    }

    return 0;
}

static int exec_command_list(UmkCtx *ctx, StrVec *cmds, int parallel_allowed) {
    for (int i = 0; i < cmds->count; i++) {
        char exp[MAX_LINE];
        expand(ctx, cmds->items[i], exp, sizeof(exp));

        char line[MAX_LINE];
        strncpy(line, exp, sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;
        trim(line);

        if (is_blank(line)) continue;

        int ret;

        if (strncmp(line, "call ", 5) == 0) {
            char *targets = line + 5;
            trim(targets);

            char *saveptr;
            char *tok = strtok_r(targets, " ", &saveptr);

            while (tok) {
                ret = execute_target(ctx, tok, parallel_allowed);
                if (ret != 0) return ret;
                tok = strtok_r(NULL, " ", &saveptr);
            }
        } else {
            ret = execute_shell_safe(ctx, line);
            if (ret != 0) return ret;
        }
    }

    return 0;
}

static int execute_command_by_name(UmkCtx *ctx, const char *name, int parallel_allowed) {
    Command *c = find_command(ctx, name);
    if (!c) {
        char msg[MAX_LINE];
        snprintf(msg, sizeof(msg), "Unknown command: %s", name);
        print_color(ctx, COLOR_RED, msg);
        return 1;
    }

    int ret = exec_flags_of_type(ctx, c, 0, parallel_allowed);
    if (ret != 0) return ret;

    ret = exec_command_list(ctx, &c->commands, parallel_allowed);
    if (ret != 0) return ret;

    return exec_flags_of_type(ctx, c, 1, parallel_allowed);
}

static int execute_serial_rule(UmkCtx *ctx, const char *target) {
    Rule *r = find_rule(ctx, target);
    if (!r) {
        char msg[MAX_LINE];
        snprintf(msg, sizeof(msg), "Unknown rule target: %s", target);
        print_color(ctx, COLOR_RED, msg);
        return 1;
    }

    StrVec actual_deps;
    strvec_init(&actual_deps);
    resolve_rule_deps(r, target, &actual_deps);

    for (int i = 0; i < actual_deps.count; i++) {
        int ret = execute_target(ctx, actual_deps.items[i], 0);
        if (ret != 0) {
            strvec_free(&actual_deps);
            return ret;
        }
    }

    if (!needs_rebuild(ctx, target, &actual_deps) && !ctx->dry_run) {
        strvec_free(&actual_deps);
        return 0;
    }

    for (int i = 0; i < r->commands.count; i++) {
        char expanded[MAX_LINE];
        expand_autovars(r->commands.items[i], target, &actual_deps, expanded, sizeof(expanded));

        char final_cmd[MAX_LINE];
        expand(ctx, expanded, final_cmd, sizeof(final_cmd));

        int ret = execute_shell_safe(ctx, final_cmd);
        if (ret != 0) {
            strvec_free(&actual_deps);
            return ret;
        }
    }

    if (!ctx->dry_run) {
        mark_rebuilt(ctx, target, &actual_deps);
    }

    strvec_free(&actual_deps);
    return 0;
}

static int execute_target(UmkCtx *ctx, const char *target, int parallel_allowed) {
    char clean[MAX_LINE];

    if (strlen(target) >= MAX_LINE) return 1;

    strcpy(clean, target);
    trim(clean);

    if (strlen(clean) == 0) return 0;

    Command *c = find_command(ctx, clean);
    if (c) {
        return execute_command_by_name(ctx, clean, parallel_allowed);
    }

    Rule *r = find_rule(ctx, clean);
    if (r) {
        if (parallel_allowed && ctx->parallel_jobs > 1) {
            return execute_parallel_rule(ctx, clean);
        }
        return execute_serial_rule(ctx, clean);
    }

    if (access(clean, F_OK) == 0) return 0;

    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), "Unknown target: %s", clean);
    print_color(ctx, COLOR_RED, msg);
    return 1;
}

static int get_cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int)n;
#endif

    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) return 1;

    int count = 0;
    char line[256];

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "processor", 9) == 0) count++;
    }

    fclose(fp);
    return count > 0 ? count : 1;
}

static void add_ready_job(UmkCtx *ctx, Job *job) {
    if (job->state != 0 || job->queued) return;

    if (ctx->ready_queue_size >= ctx->ready_queue_capacity) {
        ctx->ready_queue_capacity = ctx->ready_queue_capacity ? ctx->ready_queue_capacity * 2 : 16;
        ctx->ready_queue = xrealloc(ctx->ready_queue, (size_t)ctx->ready_queue_capacity * sizeof(Job *));
    }

    ctx->ready_queue[ctx->ready_queue_size++] = job;
    job->queued = 1;
}

static Job *pop_ready_job(UmkCtx *ctx) {
    if (ctx->ready_queue_size == 0) return NULL;

    Job *job = ctx->ready_queue[--ctx->ready_queue_size];
    job->queued = 0;
    return job;
}

static Job *find_job_by_pid(UmkCtx *ctx, pid_t pid) {
    for (Job *j = ctx->all_jobs; j; j = j->next) {
        if (j->state == 1 && j->pid == pid) return j;
    }
    return NULL;
}

static Job *find_job_by_target(UmkCtx *ctx, const char *target) {
    for (Job *j = ctx->all_jobs; j; j = j->next) {
        if (strcmp(j->target, target) == 0) return j;
    }
    return NULL;
}

static void mark_dependency_done(UmkCtx *ctx, Job *job) {
    for (Job *j = ctx->all_jobs; j; j = j->next) {
        if (j->state != 0 || j->deps_remaining <= 0) continue;

        for (int i = 0; i < j->deps.count; i++) {
            if (strcmp(j->deps.items[i], job->target) == 0) {
                j->deps_remaining--;
                if (j->deps_remaining == 0) add_ready_job(ctx, j);
                break;
            }
        }
    }
}

static void free_job_graph(UmkCtx *ctx) {
    Job *j = ctx->all_jobs;

    while (j) {
        Job *next = j->next;

        free(j->target);
        strvec_free(&j->deps);
        strvec_free(&j->pre_commands);
        strvec_free(&j->final_cmds);
        free(j);

        j = next;
    }

    ctx->all_jobs = NULL;

    free(ctx->ready_queue);
    ctx->ready_queue = NULL;

    ctx->ready_queue_size = 0;
    ctx->ready_queue_capacity = 0;
    ctx->jobs_running = 0;
    ctx->build_failed = 0;
}

static Job *build_rule_graph(UmkCtx *ctx, const char *target, StrVec *path, int *err) {
    for (int i = 0; i < path->count; i++) {
        if (strcmp(path->items[i], target) == 0) {
            fprintf(stderr, "Circular dependency: ");
            for (int j = 0; j < path->count; j++) {
                fprintf(stderr, "%s -> ", path->items[j]);
            }
            fprintf(stderr, "%s\n", target);
            *err = 1;
            return NULL;
        }
    }

    Job *existing = find_job_by_target(ctx, target);
    if (existing) return existing;

    if (find_command(ctx, target)) {
        fprintf(stderr, "Internal graph error: command '%s' passed as rule dependency\n", target);
        *err = 1;
        return NULL;
    }

    Rule *r = find_rule(ctx, target);

    Job *job = xmalloc(sizeof(Job));
    memset(job, 0, sizeof(*job));

    job->target = xstrdup(target);
    strvec_init(&job->deps);
    strvec_init(&job->pre_commands);
    strvec_init(&job->final_cmds);
    job->state = 0;
    job->queued = 0;
    job->pid = 0;
    job->next = ctx->all_jobs;
    ctx->all_jobs = job;

    if (!r) {
        if (access(target, F_OK) == 0) {
            job->kind = JOB_FILE;
            job->deps_remaining = 0;
            return job;
        }

        char msg[MAX_LINE];
        snprintf(msg, sizeof(msg), "Unknown target: %s", target);
        print_color(ctx, COLOR_RED, msg);
        *err = 1;
        return NULL;
    }

    job->kind = JOB_RULE;
    job->rule = r;

    strvec_add(path, target);

    StrVec deps;
    strvec_init(&deps);
    resolve_rule_deps(r, target, &deps);

    for (int i = 0; i < deps.count; i++) {
        const char *dep = deps.items[i];

        if (find_command(ctx, dep)) {
            if (!strvec_contains(&job->pre_commands, dep)) {
                strvec_add(&job->pre_commands, dep);
            }
            job->forced = 1;
            continue;
        }

        Job *dj = build_rule_graph(ctx, dep, path, err);
        if (!dj) {
            strvec_pop(path);
            strvec_free(&deps);
            return NULL;
        }

        if (!strvec_contains(&job->deps, dep)) {
            strvec_add(&job->deps, dep);
            job->deps_remaining++;
        }
    }

    strvec_pop(path);
    strvec_free(&deps);
    return job;
}

static int job_prepare_commands(UmkCtx *ctx, Job *job) {
    strvec_clear(&job->final_cmds);

    if (!job->rule) return 0;

    for (int i = 0; i < job->rule->commands.count; i++) {
        char expanded[MAX_LINE];
        expand_autovars(job->rule->commands.items[i], job->target, &job->deps, expanded, sizeof(expanded));

        char final_cmd[MAX_LINE];
        expand(ctx, expanded, final_cmd, sizeof(final_cmd));

        strvec_add(&job->final_cmds, final_cmd);
    }

    return 0;
}

static void handle_sigint(int sig) {
    (void)sig;

    if (active_ctx) {
        active_ctx->interrupted = 1;

        for (Job *j = active_ctx->all_jobs; j; j = j->next) {
            if (j->state == 1 && j->pid > 0) {
                kill(j->pid, SIGTERM);
            }
        }
    }
}

static int execute_parallel_rule(UmkCtx *ctx, const char *target_name) {
    char clean[MAX_LINE];

    if (strlen(target_name) >= MAX_LINE) return 1;

    strcpy(clean, target_name);
    trim(clean);

    if (strlen(clean) == 0) return 0;

    UmkCtx *old_active = active_ctx;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);

    active_ctx = ctx;

    ctx->all_jobs = NULL;
    ctx->ready_queue = NULL;
    ctx->ready_queue_size = 0;
    ctx->ready_queue_capacity = 0;
    ctx->jobs_running = 0;
    ctx->build_failed = 0;
    ctx->interrupted = 0;

    strvec_init(&ctx->done_commands);

    int err = 0;
    StrVec path;
    strvec_init(&path);

    Job *root = build_rule_graph(ctx, clean, &path, &err);
    strvec_free(&path);

    if (!root) {
        free_job_graph(ctx);
        strvec_free(&ctx->done_commands);
        active_ctx = old_active;
        return 1;
    }

    for (Job *j = ctx->all_jobs; j; j = j->next) {
        if (j->deps_remaining == 0) add_ready_job(ctx, j);
    }

    while ((ctx->ready_queue_size > 0 || ctx->jobs_running > 0) && !ctx->build_failed && !ctx->interrupted) {
        while (ctx->ready_queue_size > 0 && ctx->jobs_running < ctx->parallel_jobs && !ctx->build_failed && !ctx->interrupted) {
            Job *job = pop_ready_job(ctx);
            if (!job || job->state != 0) continue;

            if (job->pre_commands.count > 0) {
                for (int i = 0; i < job->pre_commands.count; i++) {
                    const char *cname = job->pre_commands.items[i];

                    if (!strvec_contains(&ctx->done_commands, cname)) {
                        int rc = execute_command_by_name(ctx, cname, 0);
                        if (rc != 0) {
                            ctx->build_failed = 1;
                            break;
                        }
                        strvec_add(&ctx->done_commands, cname);
                    }
                }

                if (ctx->build_failed) break;
            }

            if (job->kind == JOB_FILE) {
                job->state = 2;
                mark_dependency_done(ctx, job);
                continue;
            }

            int dirty = job->forced;
            if (!dirty) dirty = needs_rebuild(ctx, job->target, &job->deps);

            if (!dirty && !ctx->dry_run) {
                job->state = 2;
                mark_dependency_done(ctx, job);
                continue;
            }

            if (job_prepare_commands(ctx, job) != 0) {
                ctx->build_failed = 1;
                break;
            }

            if (ctx->dry_run) {
                for (int i = 0; i < job->final_cmds.count; i++) {
                    printf("%s\n", job->final_cmds.items[i]);
                }

                job->state = 2;
                mark_dependency_done(ctx, job);
                continue;
            }

            job->state = 1;
            ctx->jobs_running++;

            pid_t pid = fork();
            if (pid == 0) {
                struct sigaction child_sa;
                memset(&child_sa, 0, sizeof(child_sa));
                child_sa.sa_handler = SIG_DFL;
                sigaction(SIGINT, &child_sa, NULL);
                active_ctx = NULL;

                int rc = 0;
                for (int i = 0; i < job->final_cmds.count; i++) {
                    rc = execute_shell_safe(ctx, job->final_cmds.items[i]);
                    if (rc != 0) break;
                }

                _exit(rc & 255);
            } else if (pid > 0) {
                job->pid = pid;
            } else {
                perror("fork");
                ctx->jobs_running--;
                job->state = 0;
                ctx->build_failed = 1;
                break;
            }
        }

        if (ctx->build_failed || ctx->interrupted) break;

        if (ctx->jobs_running > 0) {
            int status;
            pid_t done = waitpid(-1, &status, 0);

            if (done > 0) {
                Job *job = find_job_by_pid(ctx, done);
                ctx->jobs_running--;

                if (job) {
                    job->pid = 0;

                    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                        job->state = 2;
                        mark_rebuilt(ctx, job->target, &job->deps);
                        mark_dependency_done(ctx, job);
                    } else {
                        job->state = 2;
                        ctx->build_failed = 1;

                        char msg[MAX_LINE];
                        if (WIFEXITED(status)) {
                            snprintf(msg, sizeof(msg), "Job %s failed with exit code %d", job->target, WEXITSTATUS(status));
                        } else {
                            snprintf(msg, sizeof(msg), "Job %s terminated abnormally", job->target);
                        }
                        print_color(ctx, COLOR_RED, msg);
                    }
                }
            } else if (done < 0) {
                if (errno == EINTR) continue;
                perror("waitpid");
                ctx->build_failed = 1;
            }
        }
    }

    if (ctx->jobs_running > 0) {
        for (Job *j = ctx->all_jobs; j; j = j->next) {
            if (j->state == 1 && j->pid > 0) {
                kill(j->pid, SIGTERM);
            }
        }

        while (ctx->jobs_running > 0) {
            int status;
            pid_t done = waitpid(-1, &status, 0);

            if (done > 0) {
                ctx->jobs_running--;
                Job *job = find_job_by_pid(ctx, done);
                if (job) {
                    job->pid = 0;
                    job->state = 2;
                }
            } else if (done < 0) {
                if (errno == EINTR) continue;
                break;
            }
        }
    }

    int ret = (ctx->build_failed || ctx->interrupted) ? 1 : 0;

    free_job_graph(ctx);
    strvec_free(&ctx->done_commands);

    active_ctx = old_active;
    return ret;
}

static void free_all(UmkCtx *ctx) {
    Rule *r = ctx->rules;
    while (r) {
        Rule *next = r->next;
        free(r->target);
        strvec_free(&r->deps);
        strvec_free(&r->commands);
        free(r);
        r = next;
    }

    Command *c = ctx->commands;
    while (c) {
        Command *next = c->next;
        strvec_free(&c->commands);

        Flag *f = c->flags;
        while (f) {
            Flag *nextf = f->next;
            strvec_free(&f->commands);
            free(f);
            f = nextf;
        }

        free(c);
        c = next;
    }

    Variable *v = ctx->variables;
    while (v) {
        Variable *next = v->next;
        free(v->value);
        free(v);
        v = next;
    }

    CacheEntry *ce = ctx->cache;
    while (ce) {
        CacheEntry *next = ce->next;
        free(ce->path);
        free(ce);
        ce = next;
    }

    RuntimeHash *rh = ctx->rt_hashes;
    while (rh) {
        RuntimeHash *next = rh->next;
        free(rh->path);
        free(rh);
        rh = next;
    }
}

#define UMK_VERSION "1.0.0"

static void print_usage(FILE *out) {
    fprintf(out,
        "Usage: umk [options] <target> [flags...]\n"
        "\n"
        "Options:\n"
        "  -j N, -jN, --jobs N, --jobs=N   Run N jobs in parallel\n"
        "  --jobs auto                      Set job count to CPU count\n"
        "  -n, --dry-run                    Show commands without executing\n"
        "  --no-color                       Disable colored output\n"
        "  -h, --help                       Show this help\n"
        "  -v, --version                    Show version\n"
        "\n"
        "Flags after the target are passed to UMK command flags.\n");
}

static void print_version(FILE *out) {
    fprintf(out, "umk %s\n", UMK_VERSION);
}

static int parse_job_count(UmkCtx *ctx, const char *value) {
    if (strcmp(value, "auto") == 0) {
        ctx->parallel_auto = 1;
        ctx->j_from_cmdline = 0;
        return 0;
    }

    int n = atoi(value);
    if (n <= 0) {
        fprintf(stderr, "error: invalid job count '%s'\n", value);
        return -1;
    }

    ctx->parallel_jobs = n;
    ctx->j_from_cmdline = 1;
    return 0;
}

int main(int argc, char **argv) {
    UmkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.parallel_jobs = 1;
    ctx.use_color = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout);
            return 0;
        }

        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version(stdout);
            return 0;
        }
    }

    char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--jobs") == 0) {
                i++;
            }
            continue;
        }

        target = argv[i];
        break;
    }

    if (!target) {
        print_usage(stderr);
        return 1;
    }

    ctx.global_flags = xmalloc((size_t)(argc > 1 ? argc : 1) * sizeof(char *));
    ctx.global_flag_count = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i] == target) continue;

        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 ||
            strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            continue;
        }

        if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--jobs") == 0) {
            if (i + 1 < argc) {
                if (parse_job_count(&ctx, argv[++i]) != 0) {
                    free(ctx.global_flags);
                    return 1;
                }
            } else {
                fprintf(stderr, "error: %s requires an argument\n", argv[i]);
                free(ctx.global_flags);
                return 1;
            }
        } else if (strncmp(argv[i], "-j", 2) == 0 && isdigit((unsigned char)argv[i][2])) {
            if (parse_job_count(&ctx, argv[i] + 2) != 0) {
                free(ctx.global_flags);
                return 1;
            }
        } else if (strncmp(argv[i], "--jobs=", 7) == 0) {
            if (parse_job_count(&ctx, argv[i] + 7) != 0) {
                free(ctx.global_flags);
                return 1;
            }
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0) {
            ctx.dry_run = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            ctx.use_color = 0;
        } else {
            ctx.global_flags[ctx.global_flag_count++] = argv[i];
        }
    }

    load_cache(&ctx);
    parse_umkfile(&ctx, "UMK");

    if (!ctx.j_from_cmdline) {
        if (ctx.parallel_auto) {
            ctx.parallel_jobs = get_cpu_count();
        }
    }

    if (ctx.parallel_jobs < 1) ctx.parallel_jobs = 1;

    int ret = execute_target(&ctx, target, ctx.parallel_jobs > 1);

    save_cache(&ctx);

    free(ctx.global_flags);
    free_all(&ctx);

    return ret;
}
