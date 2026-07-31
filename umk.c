#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>

#define MAX_LINE 4096
#define MAX_NAME 256
#define MAX_ARGS 128

#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_RESET   "\033[0m"

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
        v->items = realloc(v->items, v->capacity * sizeof(char*));
        if (!v->items) { perror("realloc"); exit(1); }
    }
    v->items[v->count++] = strdup(str);
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

typedef struct Job {
    char *target;
    Rule *rule;
    Command *cmd;
    StrVec *deps;
    int deps_remaining;
    int state;
    pid_t pid;
    struct Job *next;
} Job;

typedef struct CacheEntry {
    char path[MAX_LINE];
    unsigned long long hash;
    struct CacheEntry *next;
} CacheEntry;

typedef struct RuntimeHash {
    char path[MAX_LINE];
    unsigned long long hash;
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
} UmkCtx;

static UmkCtx *active_ctx = NULL;

void trim(char *str);
int is_blank(const char *str);
void expand(UmkCtx *ctx, const char *str, char *out, size_t out_size);
void add_variable(UmkCtx *ctx, const char *name, const char *value);
char *get_variable(UmkCtx *ctx, const char *name);
void parse_umkfile(UmkCtx *ctx, const char *filename);
int execute_serial(UmkCtx *ctx, const char *target);
int execute_parallel(UmkCtx *ctx, const char *target);
int execute_shell_safe(UmkCtx *ctx, const char *cmd_line);
int needs_rebuild(UmkCtx *ctx, const char *target, StrVec *deps);
int match_pattern(const char *name, const char *pattern);
int apply_pattern(const char *target, const char *pattern, char *out, size_t out_size);
void wildcard(const char *pattern, char *out, size_t out_size);
void shell_cmd(const char *cmd, char *out, size_t out_size);
void print_color(UmkCtx *ctx, const char *color, const char *msg);
void free_all(UmkCtx *ctx);
static int exec_command_list(UmkCtx *ctx, StrVec *cmds, int parallel);
static Flag *parse_flag_line(const char *line);
int get_cpu_count(void);
Job *build_job_graph(UmkCtx *ctx, const char *target, StrVec *path);
void add_ready_job(UmkCtx *ctx, Job *job);
Job *pop_ready_job(UmkCtx *ctx);
Job *find_job_by_pid(UmkCtx *ctx, pid_t pid);
Job *find_job_by_target(UmkCtx *ctx, const char *target);
void mark_dependency_done(UmkCtx *ctx, Job *job);
void free_job_graph(UmkCtx *ctx);
int run_job(UmkCtx *ctx, Job *job);
void handle_sigint(int sig);
void expand_autovars(const char *cmd, const char *target, StrVec *deps, char *out, size_t out_size);

unsigned long long hash_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned long long hash = 14695981039346656037ULL;
    unsigned char buf[8192];
    size_t bytes;
    while ((bytes = fread(buf, 1, sizeof(buf), f)) > 0) {
        for (size_t i = 0; i < bytes; i++) {
            hash ^= buf[i];
            hash *= 1099511628211ULL;
        }
    }
    fclose(f);
    return hash;
}

void load_cache(UmkCtx *ctx) {
    ctx->cache = NULL;
    FILE *f = fopen(".umk_cache", "r");
    if (!f) return;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (is_blank(line)) continue;
        char path[MAX_LINE];
        unsigned long long hash_val;
        if (sscanf(line, "%s %llu", path, &hash_val) == 2) {
            CacheEntry *entry = malloc(sizeof(CacheEntry));
            if (entry) {
                strncpy(entry->path, path, MAX_LINE - 1);
                entry->path[MAX_LINE - 1] = 0;
                entry->hash = hash_val;
                entry->next = ctx->cache;
                ctx->cache = entry;
            }
        }
    }
    fclose(f);
}

void save_cache(UmkCtx *ctx) {
    FILE *f = fopen(".umk_cache", "w");
    if (!f) return;
    CacheEntry *curr = ctx->cache;
    while (curr) {
        fprintf(f, "%s %llu\n", curr->path, curr->hash);
        curr = curr->next;
    }
    fclose(f);
}

unsigned long long get_cached_hash(UmkCtx *ctx, const char *path) {
    for (CacheEntry *curr = ctx->cache; curr; curr = curr->next)
        if (strcmp(curr->path, path) == 0) return curr->hash;
    return 0;
}

void update_cached_hash(UmkCtx *ctx, const char *path, unsigned long long hash_val) {
    for (CacheEntry *curr = ctx->cache; curr; curr = curr->next) {
        if (strcmp(curr->path, path) == 0) {
            curr->hash = hash_val;
            return;
        }
    }
    CacheEntry *entry = malloc(sizeof(CacheEntry));
    if (entry) {
        strncpy(entry->path, path, MAX_LINE - 1);
        entry->path[MAX_LINE - 1] = 0;
        entry->hash = hash_val;
        entry->next = ctx->cache;
        ctx->cache = entry;
    }
}

unsigned long long get_current_hash(UmkCtx *ctx, const char *path) {
    for (RuntimeHash *r = ctx->rt_hashes; r; r = r->next) {
        if (strcmp(r->path, path) == 0) return r->hash;
    }
    unsigned long long h = hash_file(path);
    RuntimeHash *nr = malloc(sizeof(RuntimeHash));
    if (nr) {
        strncpy(nr->path, path, MAX_LINE - 1);
        nr->path[MAX_LINE - 1] = 0;
        nr->hash = h;
        nr->next = ctx->rt_hashes;
        ctx->rt_hashes = nr;
    }
    return h;
}

int needs_rebuild(UmkCtx *ctx, const char *target, StrVec *deps) {
    if (access(target, F_OK) != 0) return 1;

    unsigned long long target_curr = get_current_hash(ctx, target);
    unsigned long long target_cached = get_cached_hash(ctx, target);
    if (target_curr == 0 || target_cached == 0 || target_curr != target_cached) {
        return 1;
    }

    for (int i = 0; i < deps->count; i++) {
        const char *dep = deps->items[i];
        if (access(dep, F_OK) != 0) return 1;

        unsigned long long dep_curr = get_current_hash(ctx, dep);
        unsigned long long dep_cached = get_cached_hash(ctx, dep);
        if (dep_curr == 0 || dep_cached == 0 || dep_curr != dep_cached) {
            return 1;
        }
    }
    return 0;
}

void mark_rebuilt(UmkCtx *ctx, const char *target, StrVec *deps) {
    update_cached_hash(ctx, target, hash_file(target));
    for (int i = 0; i < deps->count; i++) {
        update_cached_hash(ctx, deps->items[i], hash_file(deps->items[i]));
    }
}

void trim(char *str) {
    char *s = str;
    while (isspace((unsigned char)*s)) s++;
    if (*s == 0) { str[0] = 0; return; }
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) e--;
    memmove(str, s, e - s + 1);
    str[e - s + 1] = 0;
}

int is_blank(const char *str) {
    while (*str) if (!isspace((unsigned char)*str++)) return 0;
    return 1;
}

void print_color(UmkCtx *ctx, const char *color, const char *msg) {
    if (ctx->use_color && isatty(STDERR_FILENO))
        fprintf(stderr, "%s%s%s\n", color, msg, COLOR_RESET);
    else
        fprintf(stderr, "%s\n", msg);
}

void add_variable(UmkCtx *ctx, const char *name, const char *value) {
    for (Variable *v = ctx->variables; v; v = v->next) {
        if (strcmp(v->name, name) == 0) {
            free(v->value);
            v->value = strdup(value);
            return;
        }
    }
    Variable *nv = malloc(sizeof(Variable));
    if (!nv) { perror("malloc"); exit(1); }
    strncpy(nv->name, name, MAX_NAME - 1);
    nv->name[MAX_NAME - 1] = 0;
    nv->value = strdup(value);
    nv->next = ctx->variables;
    ctx->variables = nv;
}

char *get_variable(UmkCtx *ctx, const char *name) {
    for (Variable *v = ctx->variables; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v->value;
    return NULL;
}

void wildcard(const char *pattern, char *out, size_t out_size) {
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

void shell_cmd(const char *cmd, char *out, size_t out_size) {
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

static int match_pattern_helper(const char *name, const char *pattern) {
    const char *pp = pattern, *nn = name;
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
        pp++; nn++;
    }
    return *nn == 0;
}

int match_pattern(const char *name, const char *pattern) {
    return match_pattern_helper(name, pattern);
}

int apply_pattern(const char *target, const char *pattern, char *out, size_t out_size) {
    out[0] = 0;
    char p[MAX_LINE];
    if (strlen(pattern) >= MAX_LINE) return 0;
    strcpy(p, pattern);
    for (char *x = p; *x; x++) if (*x == '%') *x = '*';
    char *star = strchr(p, '*');
    if (!star) {
        strncpy(out, pattern, out_size - 1);
        out[out_size - 1] = 0;
        return 1;
    }
    int pre = star - p;
    int suf = strlen(p) - (pre + 1);
    if (strncmp(target, p, pre) != 0) return 0;
    int tlen = strlen(target);
    if (tlen < pre + suf) return 0;
    if (suf > 0 && strcmp(target + tlen - suf, p + pre + 1) != 0) return 0;
    int stem_len = tlen - pre - suf;
    if ((size_t)stem_len >= out_size) stem_len = out_size - 1;
    strncpy(out, target + pre, stem_len);
    out[stem_len] = 0;
    return 1;
}

void expand(UmkCtx *ctx, const char *str, char *out, size_t out_size) {
    size_t len = 0;
    out[0] = 0;
    const char *p = str;
    while (*p && len < out_size - 1) {
        if (*p == '$' && p[1] == '(') {
            const char *end = strchr(p, ')');
            if (!end) break;
            char inner[1024];
            size_t ilen = end - p - 2;
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
                            if (f_len == 0) matches = 1;
                            else if (t_len >= f_len && strcmp(tok + t_len - f_len, from) == 0) matches = 1;

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

void expand_autovars(const char *cmd, const char *target, StrVec *deps, char *out, size_t out_size) {
    size_t len = 0;
    out[0] = 0;
    const char *p = cmd;
    while (*p && len < out_size - 1) {
        if (*p == '$') {
            if (p[1] == '@') {
                size_t tlen = strlen(target);
                if (len + tlen >= out_size) tlen = out_size - len - 1;
                memcpy(out + len, target, tlen);
                len += tlen; out[len] = 0;
                p += 2; continue;
            } else if (p[1] == '<') {
                if (deps && deps->count > 0) {
                    size_t dlen = strlen(deps->items[0]);
                    if (len + dlen >= out_size) dlen = out_size - len - 1;
                    memcpy(out + len, deps->items[0], dlen);
                    len += dlen; out[len] = 0;
                }
                p += 2; continue;
            } else if (p[1] == '^') {
                for (int i = 0; i < deps->count; i++) {
                    if (i > 0) {
                        if (len < out_size - 1) out[len++] = ' ';
                        out[len] = 0;
                    }
                    size_t dlen = strlen(deps->items[i]);
                    if (len + dlen >= out_size) dlen = out_size - len - 1;
                    memcpy(out + len, deps->items[i], dlen);
                    len += dlen; out[len] = 0;
                }
                p += 2; continue;
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
    if (strncmp(copy, "-fg(", 4) != 0 && strncmp(copy, "+fg(", 4) != 0)
        return NULL;
    Flag *f = malloc(sizeof(Flag));
    strvec_init(&f->commands);
    f->next = NULL;
    f->type = (copy[0] == '-') ? 0 : 1;
    char *start = strchr(copy, '(');
    if (!start) { free(f); return NULL; }
    start++;
    char *end = strchr(start, ')');
    if (!end) { free(f); return NULL; }
    int len = end - start;
    if (len >= MAX_NAME) len = MAX_NAME - 1;
    strncpy(f->name, start, len);
    f->name[len] = 0;
    return f;
}

void parse_umkfile(UmkCtx *ctx, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) { print_color(ctx, COLOR_RED, "No UMK file"); exit(1); }
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
            cond_ptr++;
            if (skip_depth > 0) { skip_depth++; continue; }
            char exp_line[MAX_LINE];
            expand(ctx, line + 3, exp_line, sizeof(exp_line));
            char *eq = strstr(exp_line, "==");
            int result = 0;
            if (eq) {
                *eq = 0;
                char *left = exp_line;
                char *right = eq + 2;
                trim(left); trim(right);
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
            if (strcmp(p, "-auto") == 0) ctx->parallel_auto = 1;
            else if (*p == '-') { int n = atoi(p + 1); if (n > 0) ctx->parallel_jobs = n; }
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
            char *name = line, *val = eq + 1;
            trim(name); trim(val);
            char exp_val[MAX_LINE];
            expand(ctx, val, exp_val, sizeof(exp_val));
            if (is_append) {
                char *old = get_variable(ctx, name);
                char new_val[MAX_LINE * 2];
                if (old) snprintf(new_val, sizeof(new_val), "%s %s", old, exp_val);
                else snprintf(new_val, sizeof(new_val), "%s", exp_val);
                add_variable(ctx, name, new_val);
            } else {
                add_variable(ctx, name, exp_val);
            }
            continue;
        }

        if (colon && !in_flags && !cur_cmd) {
            *colon = 0;
            char *target = line, *deps = colon + 1;
            trim(target); trim(deps);
            if (strlen(deps) == 0) {
                Command *cmd = malloc(sizeof(Command));
                if (!cmd) { perror("malloc"); exit(1); }
                strncpy(cmd->name, target, MAX_NAME - 1);
                cmd->name[MAX_NAME - 1] = 0;
                strvec_init(&cmd->commands);
                cmd->flags = NULL;
                cmd->next = ctx->commands;
                ctx->commands = cmd;
                cur_cmd = cmd;
            } else {
                Rule *r = malloc(sizeof(Rule));
                if (!r) { perror("malloc"); exit(1); }
                r->target = strdup(target);
                strvec_init(&r->deps);
                strvec_init(&r->commands);
                char exp_deps[MAX_LINE];
                expand(ctx, deps, exp_deps, sizeof(exp_deps));
                char *copy = strdup(exp_deps);
                char *tok = strtok(copy, " ");
                while (tok) { strvec_add(&r->deps, tok); tok = strtok(NULL, " "); }
                free(copy);

                while (fgets(line, sizeof(line), fp)) {
                    line_num++;
                    char trimmed[MAX_LINE];
                    strcpy(trimmed, line);
                    trim(trimmed);
                    if (is_blank(trimmed)) continue;
                    if (trimmed[0] == '#') continue;
                    if (strcmp(trimmed, "eoc") == 0) break;
                    if (isspace((unsigned char)line[0])) {
                        strvec_add(&r->commands, trimmed);
                    } else {
                        fprintf(stderr, "%s:%d: error: rule command must be indented (tab or spaces)\n", filename, line_num);
                        exit(1);
                    }
                }
                r->next = ctx->rules;
                ctx->rules = r;
            }
            continue;
        }

        if (strcmp(line, "eoc") == 0) { cur_cmd = NULL; in_flags = 0; cur_flag = NULL; continue; }
        if (!cur_cmd) {
            fprintf(stderr, "%s:%d: error: statement outside of any command/rule block: '%s'\n", filename, line_num, line);
            exit(1);
        }
        if (strcmp(line, "+flags:") == 0) { in_flags = 1; continue; }
        if (in_flags) {
            if (strcmp(line, ";") == 0) { in_flags = 0; cur_flag = NULL; continue; }
            Flag *new_flag = parse_flag_line(orig);
            if (new_flag) {
                new_flag->next = cur_cmd->flags;
                cur_cmd->flags = new_flag;
                cur_flag = new_flag;
                continue;
            }
            if (strcmp(line, "eofg") == 0) { cur_flag = NULL; continue; }
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
    if (in_flags || cur_cmd) {
        fprintf(stderr, "%s:%d: error: unexpected end of file (unclosed command or flag block)\n", filename, line_num);
        exit(1);
    }
    fclose(fp);
}

int execute_shell_safe(UmkCtx *ctx, const char *cmd_line) {
    if (ctx->dry_run) {
        printf("%s\n", cmd_line);
        return 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd_line, (char *)NULL);
        perror("execl");
        exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return 1;
    } else {
        perror("fork");
        return 1;
    }
}

static int exec_command_list(UmkCtx *ctx, StrVec *cmds, int parallel_mode) {
    for (int i = 0; i < cmds->count; i++) {
        char exp[MAX_LINE];
        expand(ctx, cmds->items[i], exp, sizeof(exp));
        char line[MAX_LINE];
        strncpy(line, exp, sizeof(line) - 1);
        line[sizeof(line) - 1] = 0;
        trim(line);
        int ret;
        if (strncmp(line, "call ", 5) == 0) {
            char *targets = line + 5;
            trim(targets);
            char *saveptr;
            char *tok = strtok_r(targets, " ", &saveptr);
            while (tok) {
                if (parallel_mode && ctx->parallel_jobs > 1) {
                    ret = execute_parallel(ctx, tok);
                } else {
                    ret = execute_serial(ctx, tok);
                }
                if (ret != 0) return ret;
                tok = strtok_r(NULL, " ", &saveptr);
            }
        } else {
            ret = execute_shell_safe(ctx, line);
        }
        if (ret != 0) return ret;
    }
    return 0;
}

static int exec_flags_of_type(UmkCtx *ctx, Command *c, int type, int parallel_mode) {
    for (Flag *f = c->flags; f; f = f->next) {
        if (f->type != type) continue;
        for (int i = 0; i < ctx->global_flag_count; i++) {
            char *flag_name = ctx->global_flags[i];
            if (flag_name[0] == '-') flag_name += (flag_name[1] == '-') ? 2 : 1;
            if (strcmp(f->name, flag_name) == 0) {
                int ret = exec_command_list(ctx, &f->commands, parallel_mode);
                if (ret != 0) return ret;
                break;
            }
        }
    }
    return 0;
}

int execute_serial(UmkCtx *ctx, const char *target_name) {
    char clean[MAX_LINE];
    if (strlen(target_name) >= MAX_LINE) return 1;
    strcpy(clean, target_name);
    trim(clean);
    if (strlen(clean) == 0) return 0;

    for (Command *c = ctx->commands; c; c = c->next) {
        if (strcmp(c->name, clean) != 0) continue;
        int ret = exec_flags_of_type(ctx, c, 0, 0);
        if (ret != 0) return ret;
        ret = exec_command_list(ctx, &c->commands, 0);
        if (ret != 0) return ret;
        return exec_flags_of_type(ctx, c, 1, 0);
    }

    for (Rule *r = ctx->rules; r; r = r->next) {
        if (!match_pattern(clean, r->target)) continue;
        StrVec actual_deps;
        strvec_init(&actual_deps);
        if (strchr(r->target, '%')) {
            char stem[MAX_LINE] = "";
            if (apply_pattern(clean, r->target, stem, sizeof(stem))) {
                for (int i = 0; i < r->deps.count; i++) {
                    char buf[MAX_LINE];
                    if (strlen(r->deps.items[i]) >= MAX_LINE) continue;
                    strcpy(buf, r->deps.items[i]);
                    char res[MAX_LINE] = "";
                    int pos = 0;
                    for (char *p = buf; *p; p++) {
                        if (*p == '%') {
                            strcpy(res + pos, stem);
                            pos += strlen(stem);
                        } else {
                            res[pos++] = *p;
                            res[pos] = 0;
                        }
                    }
                    strvec_add(&actual_deps, res);
                }
            }
        } else {
            for (int i = 0; i < r->deps.count; i++) strvec_add(&actual_deps, r->deps.items[i]);
        }
        for (int i = 0; i < actual_deps.count; i++) {
            int ret = execute_serial(ctx, actual_deps.items[i]);
            if (ret != 0) { strvec_free(&actual_deps); return ret; }
        }
        if (!needs_rebuild(ctx, clean, &actual_deps) && !ctx->dry_run) { strvec_free(&actual_deps); return 0; }
        for (int i = 0; i < r->commands.count; i++) {
            char expanded[MAX_LINE];
            expand_autovars(r->commands.items[i], clean, &actual_deps, expanded, sizeof(expanded));
            char final[MAX_LINE];
            expand(ctx, expanded, final, sizeof(final));
            int ret = execute_shell_safe(ctx, final);
            if (ret != 0) { strvec_free(&actual_deps); return ret; }
        }
        mark_rebuilt(ctx, clean, &actual_deps);
        strvec_free(&actual_deps);
        return 0;
    }

    if (access(clean, F_OK) == 0) return 0;
    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), "Unknown target: %s", clean);
    print_color(ctx, COLOR_RED, msg);
    return 1;
}

int get_cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int)n;
#endif
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) return 1;
    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) if (strncmp(line, "processor", 9) == 0) count++;
    fclose(fp);
    return count > 0 ? count : 1;
}

void add_ready_job(UmkCtx *ctx, Job *job) {
    if (job->state != 0) return;
    if (ctx->ready_queue_size >= ctx->ready_queue_capacity) {
        ctx->ready_queue_capacity = ctx->ready_queue_capacity ? ctx->ready_queue_capacity * 2 : 16;
        ctx->ready_queue = realloc(ctx->ready_queue, ctx->ready_queue_capacity * sizeof(Job*));
        if (!ctx->ready_queue) { perror("realloc ready_queue"); exit(1); }
    }
    ctx->ready_queue[ctx->ready_queue_size++] = job;
}

Job *pop_ready_job(UmkCtx *ctx) {
    if (ctx->ready_queue_size == 0) return NULL;
    return ctx->ready_queue[--ctx->ready_queue_size];
}

Job *find_job_by_pid(UmkCtx *ctx, pid_t pid) {
    for (Job *j = ctx->all_jobs; j; j = j->next)
        if (j->state == 1 && j->pid == pid) return j;
    return NULL;
}

Job *find_job_by_target(UmkCtx *ctx, const char *target) {
    for (Job *j = ctx->all_jobs; j; j = j->next)
        if (strcmp(j->target, target) == 0) return j;
    return NULL;
}

void mark_dependency_done(UmkCtx *ctx, Job *job) {
    for (Job *j = ctx->all_jobs; j; j = j->next) {
        if (j->state != 0) continue;
        for (int i = 0; i < j->deps->count; i++) {
            if (strcmp(j->deps->items[i], job->target) == 0) {
                j->deps_remaining--;
                if (j->deps_remaining == 0 && j->state == 0) add_ready_job(ctx, j);
                break;
            }
        }
    }
}

void free_job_graph(UmkCtx *ctx) {
    Job *j = ctx->all_jobs;
    while (j) {
        Job *next = j->next;
        free(j->target);
        if (j->deps) { strvec_free(j->deps); free(j->deps); }
        free(j);
        j = next;
    }
    ctx->all_jobs = NULL;
    if (ctx->ready_queue) { free(ctx->ready_queue); ctx->ready_queue = NULL; }
    ctx->ready_queue_size = ctx->ready_queue_capacity = ctx->jobs_running = ctx->build_failed = 0;
}

Job *build_job_graph(UmkCtx *ctx, const char *target, StrVec *path) {
    for (int i = 0; i < path->count; i++)
        if (strcmp(path->items[i], target) == 0) {
            fprintf(stderr, "Circular dependency: ");
            for (int j = 0; j < path->count; j++) fprintf(stderr, "%s -> ", path->items[j]);
            fprintf(stderr, "%s\n", target);
            exit(1);
        }
    Job *existing = find_job_by_target(ctx, target);
    if (existing) return existing;
    Job *job = calloc(1, sizeof(Job));
    if (!job) { perror("calloc"); exit(1); }
    job->target = strdup(target);
    job->state = 0;
    job->deps = malloc(sizeof(StrVec));
    if (!job->deps) { perror("malloc"); exit(1); }
    strvec_init(job->deps);
    job->next = ctx->all_jobs;
    ctx->all_jobs = job;
    strvec_add(path, target);

    for (Command *c = ctx->commands; c; c = c->next) {
        if (strcmp(c->name, target) == 0) {
            job->cmd = c;
            job->rule = NULL;
            job->deps_remaining = 0;
            add_ready_job(ctx, job);
            strvec_clear(path);
            return job;
        }
    }
    for (Rule *r = ctx->rules; r; r = r->next) {
        if (match_pattern(target, r->target)) {
            job->rule = r;
            job->cmd = NULL;
            char stem[MAX_LINE] = "";
            int has_stem = 0;
            if (strchr(r->target, '%')) {
                has_stem = apply_pattern(target, r->target, stem, sizeof(stem));
            }
            for (int i = 0; i < r->deps.count; i++) {
                char *dep_pattern = r->deps.items[i];
                char dep_buf[MAX_LINE];
                if (has_stem) {
                    dep_buf[0] = 0;
                    int pos = 0;
                    for (char *p = dep_pattern; *p; p++) {
                        if (*p == '%') {
                            strcpy(dep_buf + pos, stem);
                            pos += strlen(stem);
                        } else {
                            dep_buf[pos++] = *p;
                            dep_buf[pos] = 0;
                        }
                    }
                } else {
                    if (strlen(dep_pattern) >= MAX_LINE) continue;
                    strcpy(dep_buf, dep_pattern);
                }
                strvec_add(job->deps, dep_buf);
                build_job_graph(ctx, dep_buf, path);
            }
            job->deps_remaining = job->deps->count;
            if (job->deps_remaining == 0) add_ready_job(ctx, job);
            strvec_clear(path);
            return job;
        }
    }
    if (access(target, F_OK) == 0) {
        job->deps_remaining = 0;
        add_ready_job(ctx, job);
        strvec_clear(path);
        return job;
    }
    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), "Unknown target: %s", target);
    print_color(ctx, COLOR_RED, msg);
    exit(1);
}

int run_job(UmkCtx *ctx, Job *job) {
    if (job->cmd) {
        int ret = exec_flags_of_type(ctx, job->cmd, 0, 1);
        if (ret != 0) return ret;
        ret = exec_command_list(ctx, &job->cmd->commands, 1);
        if (ret != 0) return ret;
        return exec_flags_of_type(ctx, job->cmd, 1, 1);
    } else if (job->rule) {
        if (!needs_rebuild(ctx, job->target, job->deps) && !ctx->dry_run) return 0;
        for (int i = 0; i < job->rule->commands.count; i++) {
            char expanded[MAX_LINE];
            expand_autovars(job->rule->commands.items[i], job->target, job->deps, expanded, sizeof(expanded));
            char final[MAX_LINE];
            expand(ctx, expanded, final, sizeof(final));
            int ret = execute_shell_safe(ctx, final);
            if (ret != 0) return ret;
        }
        mark_rebuilt(ctx, job->target, job->deps);
        return 0;
    }
    return 0;
}

void handle_sigint(int sig) {
    (void)sig;
    if (active_ctx) {
        active_ctx->interrupted = 1;
        for (Job *j = active_ctx->all_jobs; j; j = j->next)
            if (j->state == 1 && j->pid > 0) kill(j->pid, SIGTERM);
    }
}

int execute_parallel(UmkCtx *ctx, const char *target_name) {
    char clean[MAX_LINE];
    if (strlen(target_name) >= MAX_LINE) return 1;
    strcpy(clean, target_name);
    trim(clean);
    if (strlen(clean) == 0) return 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    active_ctx = ctx;
    ctx->all_jobs = NULL;
    ctx->ready_queue = NULL;
    ctx->ready_queue_size = ctx->ready_queue_capacity = ctx->jobs_running = ctx->build_failed = 0;

    StrVec path;
    strvec_init(&path);
    Job *root = build_job_graph(ctx, clean, &path);
    strvec_free(&path);
    if (!root) return 1;

    while ((ctx->ready_queue_size > 0 || ctx->jobs_running > 0) && !ctx->build_failed && !ctx->interrupted) {
        while (ctx->ready_queue_size > 0 && ctx->jobs_running < ctx->parallel_jobs && !ctx->build_failed && !ctx->interrupted) {
            Job *job = pop_ready_job(ctx);
            if (!job || job->state != 0) continue;
            job->state = 1;
            ctx->jobs_running++;
            pid_t pid = fork();
            if (pid == 0) {
                int ret = run_job(ctx, job);
                exit(ret);
            }
            else if (pid > 0) job->pid = pid;
            else { perror("fork"); ctx->build_failed = 1; break; }
        }
        if (ctx->jobs_running > 0 && !ctx->build_failed && !ctx->interrupted) {
            int status;
            pid_t done = waitpid(-1, &status, 0);
            if (done > 0) {
                Job *job = find_job_by_pid(ctx, done);
                if (job) {
                    ctx->jobs_running--;
                    job->state = 2;
                    if (WIFEXITED(status)) {
                        int exit_code = WEXITSTATUS(status);
                        if (exit_code != 0) {
                            ctx->build_failed = 1;
                            char msg[MAX_LINE];
                            snprintf(msg, sizeof(msg), "Job %s failed with exit code %d", job->target, exit_code);
                            print_color(ctx, COLOR_RED, msg);
                        }
                    } else {
                        ctx->build_failed = 1;
                        char msg[MAX_LINE];
                        snprintf(msg, sizeof(msg), "Job %s terminated abnormally", job->target);
                        print_color(ctx, COLOR_RED, msg);
                    }
                    mark_dependency_done(ctx, job);
                }
            } else if (done < 0) {
                if (errno == EINTR) continue;
                perror("waitpid");
                ctx->build_failed = 1;
            }
        }
    }
    int ret = (ctx->build_failed || ctx->interrupted) ? 1 : 0;
    free_job_graph(ctx);
    active_ctx = NULL;
    return ret;
}

void free_all(UmkCtx *ctx) {
    Rule *r = ctx->rules;
    while (r) { Rule *next = r->next; free(r->target); strvec_free(&r->deps); strvec_free(&r->commands); free(r); r = next; }
    Command *c = ctx->commands;
    while (c) { Command *next = c->next; strvec_free(&c->commands); Flag *f = c->flags; while (f) { Flag *nextf = f->next; strvec_free(&f->commands); free(f); f = nextf; } free(c); c = next; }
    Variable *v = ctx->variables;
    while (v) { Variable *next = v->next; free(v->value); free(v); v = next; }
    CacheEntry *ce = ctx->cache;
    while (ce) { CacheEntry *next = ce->next; free(ce); ce = next; }
    RuntimeHash *rh = ctx->rt_hashes;
    while (rh) { RuntimeHash *next = rh->next; free(rh); rh = next; }
}

int main(int argc, char **argv) {
    UmkCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.parallel_jobs = 1;
    ctx.use_color = 1;

    if (argc < 2) { print_color(&ctx, COLOR_YELLOW, "Usage: umk <command> [flags...] [-j N]"); return 1; }

    ctx.global_flag_count = 0;
    int max_flags = argc > 2 ? argc - 2 : 1;
    ctx.global_flags = malloc(max_flags * sizeof(char*));
    if (!ctx.global_flags) { perror("malloc"); return 1; }

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-j") == 0) {
            if (i + 1 < argc) {
                ctx.parallel_jobs = atoi(argv[++i]);
                ctx.j_from_cmdline = 1;
            } else {
                fprintf(stderr, "error: -j requires an argument\n");
                free(ctx.global_flags);
                return 1;
            }
        } else if (strncmp(argv[i], "-j", 2) == 0 && isdigit((unsigned char)argv[i][2])) {
            ctx.parallel_jobs = atoi(argv[i] + 2);
            ctx.j_from_cmdline = 1;
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
    if (!ctx.j_from_cmdline) { if (ctx.parallel_auto) ctx.parallel_jobs = get_cpu_count(); }
    if (ctx.parallel_jobs < 1) ctx.parallel_jobs = 1;

    int ret;
    if (ctx.parallel_jobs > 1) ret = execute_parallel(&ctx, argv[1]);
    else ret = execute_serial(&ctx, argv[1]);

    save_cache(&ctx);
    free(ctx.global_flags);
    free_all(&ctx);
    return ret;
}
