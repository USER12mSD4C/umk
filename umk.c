// файл: umk.c
// Полностью рабочий параллельный сборщик с поддержкой -j, автоопределением ядер,
// детекцией циклов, безопасным execvp и отсутствием блокировок (как в make).

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

/* ---------- Dynamic string array ---------- */
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

/* ---------- Data structures ---------- */
typedef struct Flag {
    char name[MAX_NAME];
    int type;              // 0 = before, 1 = after
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

/* ---------- Job graph ---------- */
typedef struct Job {
    char *target;
    Rule *rule;
    Command *cmd;
    StrVec *deps;            // имена зависимостей
    int deps_remaining;
    int state;               // 0=waiting,1=running,2=done
    pid_t pid;
    int rebuild_needed;
    struct Job *next;
} Job;

/* ---------- Globals ---------- */
Rule *rules = NULL;
Command *commands = NULL;
Variable *variables = NULL;
char **global_flags = NULL;
int global_flag_count = 0;
int use_color = 1;
int dry_run = 0;

int parallel_jobs = 1;
int parallel_auto = 0;
int j_from_cmdline = 0;
Job *all_jobs = NULL;
Job **ready_queue = NULL;
int ready_queue_size = 0;
int ready_queue_capacity = 0;
int jobs_running = 0;
int build_failed = 0;
int interrupted = 0;

/* ---------- Forward declarations ---------- */
void trim(char *str);
int is_blank(const char *str);
char *expand(const char *str);
void add_variable(const char *name, const char *value);
char *get_variable(const char *name);
void parse_umkfile(const char *filename);
int execute_serial(const char *target);
int execute_parallel(const char *target);
int execute_shell_safe(const char *cmd_line);
int needs_rebuild(const char *target, StrVec *deps);
time_t get_mtime(const char *path);
int match_pattern(const char *name, const char *pattern);
char *apply_pattern(const char *target, const char *pattern);
char *wildcard(const char *pattern);
char *shell_cmd(const char *cmd);
void print_color(const char *color, const char *msg);
void free_all(void);
static int exec_command_list(StrVec *cmds, int parallel);
static Flag *parse_flag_line(const char *line);
int get_cpu_count(void);
Job *build_job_graph(const char *target, StrVec *path);
void add_ready_job(Job *job);
Job *pop_ready_job(void);
Job *find_job_by_pid(pid_t pid);
Job *find_job_by_target(const char *target);
void mark_dependency_done(Job *job);
void free_job_graph(void);
int run_job(Job *job);
void handle_sigint(int sig);
char **parse_command_line(const char *cmd_line, int *argc);
int execute_command(char **argv);
void expand_autovars(const char *cmd, const char *target, StrVec *deps, char *out, size_t out_size);

/* ---------- Utility functions (unchanged) ---------- */
time_t get_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

int needs_rebuild(const char *target, StrVec *deps) {
    time_t t = get_mtime(target);
    if (t == 0) return 1;
    for (int i = 0; i < deps->count; i++) {
        time_t d = get_mtime(deps->items[i]);
        if (d == 0 || d > t) return 1;
    }
    return 0;
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

void print_color(const char *color, const char *msg) {
    if (use_color && isatty(STDERR_FILENO))
        fprintf(stderr, "%s%s%s\n", color, msg, COLOR_RESET);
    else
        fprintf(stderr, "%s\n", msg);
}

void add_variable(const char *name, const char *value) {
    for (Variable *v = variables; v; v = v->next) {
        if (strcmp(v->name, name) == 0) {
            free(v->value);
            v->value = strdup(value);
            return;
        }
    }
    Variable *nv = malloc(sizeof(Variable));
    strcpy(nv->name, name);
    nv->value = strdup(value);
    nv->next = variables;
    variables = nv;
}

char *get_variable(const char *name) {
    for (Variable *v = variables; v; v = v->next)
        if (strcmp(v->name, name) == 0) return v->value;
    return NULL;
}

char *wildcard(const char *pattern) {
    static char res[MAX_LINE];
    res[0] = 0;
    DIR *d = opendir(".");
    if (!d) return res;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (match_pattern(e->d_name, pattern)) {
            if (strlen(res) > 0) strcat(res, " ");
            strcat(res, e->d_name);
        }
    }
    closedir(d);
    return res;
}

char *shell_cmd(const char *cmd) {
    static char res[MAX_LINE];
    res[0] = 0;
    FILE *f = popen(cmd, "r");
    if (!f) return res;
    char line[MAX_LINE];
    if (fgets(line, sizeof(line), f)) {
        trim(line);
        strcpy(res, line);
    }
    pclose(f);
    return res;
}

int match_pattern(const char *name, const char *pattern) {
    char p[MAX_LINE];
    strcpy(p, pattern);
    for (char *x = p; *x; x++) if (*x == '%') *x = '*';
    const char *pp = p, *nn = name;
    while (*pp) {
        if (*pp == '*') {
            pp++;
            if (!*pp) return 1;
            while (*nn) {
                if (match_pattern(nn, pp)) return 1;
                nn++;
            }
            return 0;
        } else if (*pp != *nn) {
            return 0;
        }
        pp++; nn++;
    }
    return *nn == 0;
}

char *apply_pattern(const char *target, const char *pattern) {
    static char res[MAX_LINE];
    char p[MAX_LINE];
    strcpy(p, pattern);
    for (char *x = p; *x; x++) if (*x == '%') *x = '*';
    char *star = strchr(p, '*');
    if (!star) { strcpy(res, pattern); return res; }
    int pre = star - p;
    int suf = strlen(p) - (pre + 1);
    if (strncmp(target, p, pre) != 0) return NULL;
    int tlen = strlen(target);
    if (tlen < pre + suf) return NULL;
    if (suf > 0 && strcmp(target + tlen - suf, p + pre + 1) != 0) return NULL;
    int stem_len = tlen - pre - suf;
    strncpy(res, target + pre, stem_len);
    res[stem_len] = 0;
    return res;
}

char *expand(const char *str) {
    static char res[MAX_LINE];
    res[0] = 0;
    const char *p = str;
    while (*p) {
        if (*p == '$' && p[1] == '(') {
            const char *end = strchr(p, ')');
            if (!end) {
                strncat(res, p, MAX_LINE - strlen(res) - 1);
                break;
            }
            int len = end - p - 2;
            char *inner = malloc(len + 1);
            strncpy(inner, p + 2, len);
            inner[len] = 0;
            char *space = strchr(inner, ' ');
            if (space) {
                *space = 0;
                char *func = inner;
                char *args = space + 1;
                if (strcmp(func, "wildcard") == 0)
                    strcat(res, wildcard(args));
                else if (strcmp(func, "shell") == 0)
                    strcat(res, shell_cmd(args));
            } else {
                char *val = get_variable(inner);
                if (val) strcat(res, val);
            }
            free(inner);
            p = end + 1;
        } else {
            strncat(res, p, 1);
            p++;
        }
    }
    return res;
}

/* ---------- Safe command execution ---------- */
char **parse_command_line(const char *cmd_line, int *argc) {
    char *line = strdup(cmd_line);
    if (!line) return NULL;
    char **argv = malloc(MAX_ARGS * sizeof(char*));
    if (!argv) { free(line); return NULL; }
    int i = 0;
    char *p = line;
    while (*p && i < MAX_ARGS - 1) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            char *start = p;
            while (*p && *p != quote) p++;
            if (*p == quote) {
                *p = 0;
                argv[i++] = strdup(start);
                p++;
            } else {
                argv[i++] = strdup(start);
            }
        } else {
            char *start = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) {
                *p = 0;
                p++;
            }
            argv[i++] = strdup(start);
        }
    }
    argv[i] = NULL;
    *argc = i;
    free(line);
    return argv;
}

int execute_command(char **argv) {
    if (dry_run) {
        for (int i = 0; argv[i]; i++) {
            if (i > 0) printf(" ");
            printf("%s", argv[i]);
        }
        printf("\n");
        return 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        perror("execvp");
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

int execute_shell_safe(const char *cmd_line) {
    int argc;
    char **argv = parse_command_line(cmd_line, &argc);
    if (!argv || argc == 0) {
        if (argv) free(argv);
        return 0;
    }
    int ret = execute_command(argv);
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return ret;
}

/* ---------- Auto-variable expansion ---------- */
void expand_autovars(const char *cmd, const char *target, StrVec *deps, char *out, size_t out_size) {
    out[0] = 0;
    int out_len = 0;
    for (const char *p = cmd; *p && out_len < out_size - 1; p++) {
        if (*p == '$' && p[1] == '@') {
            strncat(out, target, out_size - strlen(out) - 1);
            p++;
        } else if (*p == '$' && p[1] == '<') {
            if (deps && deps->count > 0)
                strncat(out, deps->items[0], out_size - strlen(out) - 1);
            p++;
        } else if (*p == '$' && p[1] == '^') {
            for (int i = 0; i < deps->count; i++) {
                if (i > 0) strncat(out, " ", out_size - strlen(out) - 1);
                strncat(out, deps->items[i], out_size - strlen(out) - 1);
            }
            p++;
        } else {
            char tmp[2] = { *p, 0 };
            strncat(out, tmp, out_size - strlen(out) - 1);
        }
    }
}

/* ---------- Parsing (unchanged) ---------- */
static Flag *parse_flag_line(const char *line) {
    char copy[MAX_LINE];
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

void parse_umkfile(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) { print_color(COLOR_RED, "No UMK file"); exit(1); }
    char line[MAX_LINE];
    Command *cur_cmd = NULL;
    Flag *cur_flag = NULL;
    int in_flags = 0;
    while (fgets(line, sizeof(line), fp)) {
        char orig[MAX_LINE];
        strcpy(orig, line);
        trim(line);
        if (is_blank(line)) continue;
        if (strncmp(line, "threadreap", 10) == 0) {
            char *p = line + 10;
            while (isspace((unsigned char)*p)) p++;
            if (strcmp(p, "-auto") == 0) parallel_auto = 1;
            else if (*p == '-') { int n = atoi(p + 1); if (n > 0) parallel_jobs = n; }
            continue;
        }
        char *eq = strchr(line, '=');
        if (eq && !in_flags && !cur_cmd) {
            *eq = 0;
            char *name = line, *val = eq + 1;
            trim(name); trim(val);
            add_variable(name, expand(val));
            continue;
        }
        char *colon = strchr(line, ':');
        if (colon && !in_flags && !cur_cmd) {
            *colon = 0;
            char *target = line, *deps = colon + 1;
            trim(target); trim(deps);
            if (strlen(deps) == 0) {
                Command *cmd = malloc(sizeof(Command));
                strcpy(cmd->name, target);
                strvec_init(&cmd->commands);
                cmd->flags = NULL;
                cmd->next = commands;
                commands = cmd;
                cur_cmd = cmd;
            } else {
                Rule *r = malloc(sizeof(Rule));
                r->target = strdup(target);
                strvec_init(&r->deps);
                strvec_init(&r->commands);
                char *exp_deps = expand(deps);
                char *copy = strdup(exp_deps);
                char *tok = strtok(copy, " ");
                while (tok) { strvec_add(&r->deps, tok); tok = strtok(NULL, " "); }
                free(copy);
                if (fgets(line, sizeof(line), fp)) {
                    char trimmed[MAX_LINE];
                    strcpy(trimmed, line);
                    trim(trimmed);
                    if (line[0] == '\t') strvec_add(&r->commands, trimmed);
                }
                while (fgets(line, sizeof(line), fp)) {
                    char trimmed[MAX_LINE];
                    strcpy(trimmed, line);
                    trim(trimmed);
                    if (strcmp(trimmed, "eoc") == 0) break;
                    if (line[0] == '\t') strvec_add(&r->commands, trimmed);
                }
                r->next = rules;
                rules = r;
            }
            continue;
        }
        if (strcmp(line, "eoc") == 0) { cur_cmd = NULL; in_flags = 0; cur_flag = NULL; continue; }
        if (!cur_cmd) continue;
        if (strcmp(line, "+flags:") == 0) { in_flags = 1; continue; }
        if (in_flags) {
            if (strcmp(line, ";") == 0) { in_flags = 0; cur_flag = NULL; continue; }
            Flag *new_flag = parse_flag_line(orig);
            if (new_flag) { new_flag->next = cur_cmd->flags; cur_cmd->flags = new_flag; cur_flag = new_flag; continue; }
            if (strcmp(line, "eofg") == 0) { cur_flag = NULL; continue; }
            if (cur_flag) strvec_add(&cur_flag->commands, line);
            continue;
        }
        if (line[0] != '\t') strvec_add(&cur_cmd->commands, line);
    }
    fclose(fp);
}

/* ---------- Execution helpers (parallel-aware) ---------- */
static int exec_command_list(StrVec *cmds, int parallel_mode) {
    for (int i = 0; i < cmds->count; i++) {
        char *exp = expand(cmds->items[i]);
        char line[MAX_LINE];
        strcpy(line, exp);
        trim(line);
        int ret;
        if (strncmp(line, "call ", 5) == 0) {
            char *target = line + 5;
            trim(target);
            if (parallel_mode && parallel_jobs > 1) {
                // В параллельном режиме call должен быть интегрирован в граф.
                // Для простоты: рекурсивно вызываем параллельную сборку подцели.
                ret = execute_parallel(target);
            } else {
                ret = execute_serial(target);
            }
        } else {
            ret = execute_shell_safe(line);
        }
        if (ret != 0) return ret;
    }
    return 0;
}

static int exec_flags_of_type(Command *c, int type, int parallel_mode) {
    for (Flag *f = c->flags; f; f = f->next) {
        if (f->type != type) continue;
        for (int i = 0; i < global_flag_count; i++) {
            char *flag_name = global_flags[i];
            if (flag_name[0] == '-') flag_name += (flag_name[1] == '-') ? 2 : 1;
            if (strcmp(f->name, flag_name) == 0) {
                int ret = exec_command_list(&f->commands, parallel_mode);
                if (ret != 0) return ret;
                break;
            }
        }
    }
    return 0;
}

/* ---------- Serial execution (original) ---------- */
int execute_serial(const char *target_name) {
    char clean[MAX_LINE];
    strcpy(clean, target_name);
    trim(clean);
    if (strlen(clean) == 0) return 0;
    for (Command *c = commands; c; c = c->next) {
        if (strcmp(c->name, clean) != 0) continue;
        int ret = exec_flags_of_type(c, 0, 0);
        if (ret != 0) return ret;
        ret = exec_command_list(&c->commands, 0);
        if (ret != 0) return ret;
        return exec_flags_of_type(c, 1, 0);
    }
    for (Rule *r = rules; r; r = r->next) {
        if (!match_pattern(clean, r->target)) continue;
        StrVec actual_deps;
        strvec_init(&actual_deps);
        if (strchr(r->target, '%')) {
            char *stem = apply_pattern(clean, r->target);
            if (stem) {
                for (int i = 0; i < r->deps.count; i++) {
                    char buf[MAX_LINE];
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
            int ret = execute_serial(actual_deps.items[i]);
            if (ret != 0) { strvec_free(&actual_deps); return ret; }
        }
        if (!needs_rebuild(clean, &actual_deps) && !dry_run) { strvec_free(&actual_deps); return 0; }
        for (int i = 0; i < r->commands.count; i++) {
            char expanded[MAX_LINE];
            expand_autovars(r->commands.items[i], clean, &actual_deps, expanded, sizeof(expanded));
            char *final = expand(expanded);
            int ret = execute_shell_safe(final);
            if (ret != 0) { strvec_free(&actual_deps); return ret; }
        }
        strvec_free(&actual_deps);
        return 0;
    }
    if (get_mtime(clean) != 0) return 0;
    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), "Unknown target: %s", clean);
    print_color(COLOR_RED, msg);
    return 1;
}

/* ---------- Parallel execution (make -j style) ---------- */
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

void add_ready_job(Job *job) {
    if (job->state != 0) return;
    if (ready_queue_size >= ready_queue_capacity) {
        ready_queue_capacity = ready_queue_capacity ? ready_queue_capacity * 2 : 16;
        ready_queue = realloc(ready_queue, ready_queue_capacity * sizeof(Job*));
        if (!ready_queue) { perror("realloc ready_queue"); exit(1); }
    }
    ready_queue[ready_queue_size++] = job;
}

Job *pop_ready_job(void) {
    if (ready_queue_size == 0) return NULL;
    return ready_queue[--ready_queue_size];
}

Job *find_job_by_pid(pid_t pid) {
    for (Job *j = all_jobs; j; j = j->next)
        if (j->state == 1 && j->pid == pid) return j;
    return NULL;
}

Job *find_job_by_target(const char *target) {
    for (Job *j = all_jobs; j; j = j->next)
        if (strcmp(j->target, target) == 0) return j;
    return NULL;
}

void mark_dependency_done(Job *job) {
    for (Job *j = all_jobs; j; j = j->next) {
        if (j->state != 0) continue;
        for (int i = 0; i < j->deps->count; i++) {
            if (strcmp(j->deps->items[i], job->target) == 0) {
                j->deps_remaining--;
                if (j->deps_remaining == 0 && j->state == 0) add_ready_job(j);
                break;
            }
        }
    }
}

void free_job_graph(void) {
    Job *j = all_jobs;
    while (j) {
        Job *next = j->next;
        free(j->target);
        if (j->deps) { strvec_free(j->deps); free(j->deps); }
        free(j);
        j = next;
    }
    all_jobs = NULL;
    if (ready_queue) { free(ready_queue); ready_queue = NULL; }
    ready_queue_size = ready_queue_capacity = jobs_running = build_failed = 0;
}

Job *build_job_graph(const char *target, StrVec *path) {
    for (int i = 0; i < path->count; i++)
        if (strcmp(path->items[i], target) == 0) {
            fprintf(stderr, "Circular dependency: ");
            for (int j = 0; j < path->count; j++) fprintf(stderr, "%s -> ", path->items[j]);
            fprintf(stderr, "%s\n", target);
            exit(1);
        }
    Job *existing = find_job_by_target(target);
    if (existing) return existing;
    Job *job = calloc(1, sizeof(Job));
    job->target = strdup(target);
    job->state = 0;
    job->deps = malloc(sizeof(StrVec));
    strvec_init(job->deps);
    job->next = all_jobs;
    all_jobs = job;
    strvec_add(path, target);
    for (Command *c = commands; c; c = c->next) {
        if (strcmp(c->name, target) == 0) {
            job->cmd = c;
            job->rule = NULL;
            job->deps_remaining = 0;
            add_ready_job(job);
            strvec_clear(path);
            return job;
        }
    }
    for (Rule *r = rules; r; r = r->next) {
        if (match_pattern(target, r->target)) {
            job->rule = r;
            job->cmd = NULL;
            char *stem = NULL;
            if (strchr(r->target, '%')) stem = apply_pattern(target, r->target);
            for (int i = 0; i < r->deps.count; i++) {
                char *dep_pattern = r->deps.items[i];
                char dep_buf[MAX_LINE];
                if (stem) {
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
                    strcpy(dep_buf, dep_pattern);
                }
                strvec_add(job->deps, dep_buf);
                build_job_graph(dep_buf, path);
            }
            free(stem);
            job->deps_remaining = job->deps->count;
            if (job->deps_remaining == 0) add_ready_job(job);
            strvec_clear(path);
            return job;
        }
    }
    if (get_mtime(target) != 0) {
        job->deps_remaining = 0;
        add_ready_job(job);
        strvec_clear(path);
        return job;
    }
    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), "Unknown target: %s", target);
    print_color(COLOR_RED, msg);
    exit(1);
}

int run_job(Job *job) {
    if (job->cmd) {
        int ret = exec_flags_of_type(job->cmd, 0, 1);
        if (ret != 0) return ret;
        ret = exec_command_list(&job->cmd->commands, 1);
        if (ret != 0) return ret;
        return exec_flags_of_type(job->cmd, 1, 1);
    } else if (job->rule) {
        if (!needs_rebuild(job->target, job->deps) && !dry_run) return 0;
        for (int i = 0; i < job->rule->commands.count; i++) {
            char expanded[MAX_LINE];
            expand_autovars(job->rule->commands.items[i], job->target, job->deps, expanded, sizeof(expanded));
            char *final = expand(expanded);
            int ret = execute_shell_safe(final);
            if (ret != 0) return ret;
        }
        return 0;
    }
    return 0;
}

void handle_sigint(int sig) {
    (void)sig;
    interrupted = 1;
    for (Job *j = all_jobs; j; j = j->next)
        if (j->state == 1 && j->pid > 0) kill(j->pid, SIGTERM);
}

int execute_parallel(const char *target_name) {
    char clean[MAX_LINE];
    strcpy(clean, target_name);
    trim(clean);
    if (strlen(clean) == 0) return 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
    all_jobs = NULL;
    ready_queue = NULL;
    ready_queue_size = ready_queue_capacity = jobs_running = build_failed = 0;
    StrVec path;
    strvec_init(&path);
    Job *root = build_job_graph(clean, &path);
    strvec_free(&path);
    if (!root) return 1;
    while ((ready_queue_size > 0 || jobs_running > 0) && !build_failed && !interrupted) {
        while (ready_queue_size > 0 && jobs_running < parallel_jobs && !build_failed && !interrupted) {
            Job *job = pop_ready_job();
            if (job->state != 0) continue;
            job->state = 1;
            jobs_running++;
            pid_t pid = fork();
            if (pid == 0) { int ret = run_job(job); exit(ret); }
            else if (pid > 0) job->pid = pid;
            else { perror("fork"); build_failed = 1; break; }
        }
        if (jobs_running > 0 && !build_failed && !interrupted) {
            int status;
            pid_t done = waitpid(-1, &status, 0);
            if (done > 0) {
                Job *job = find_job_by_pid(done);
                if (job) {
                    jobs_running--;
                    job->state = 2;
                    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                        build_failed = 1;
                        char msg[MAX_LINE];
                        snprintf(msg, sizeof(msg), "Job %s failed with exit code %d", job->target, WEXITSTATUS(status));
                        print_color(COLOR_RED, msg);
                    }
                    mark_dependency_done(job);
                }
            }
        }
    }
    int ret = (build_failed || interrupted) ? 1 : 0;
    free_job_graph();
    return ret;
}

/* ---------- Cleanup ---------- */
void free_all(void) {
    Rule *r = rules;
    while (r) { Rule *next = r->next; free(r->target); strvec_free(&r->deps); strvec_free(&r->commands); free(r); r = next; }
    Command *c = commands;
    while (c) { Command *next = c->next; strvec_free(&c->commands); Flag *f = c->flags; while (f) { Flag *nextf = f->next; strvec_free(&f->commands); free(f); f = nextf; } free(c); c = next; }
    Variable *v = variables;
    while (v) { Variable *next = v->next; free(v->value); free(v); v = next; }
}

/* ---------- Main ---------- */
int main(int argc, char **argv) {
    if (argc < 2) { print_color(COLOR_YELLOW, "Usage: umk <command> [flags...] [-j N]"); return 1; }
    global_flag_count = 0;
    global_flags = malloc((argc - 2) * sizeof(char*));
    if (!global_flags) { perror("malloc"); return 1; }
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            parallel_jobs = atoi(argv[++i]);
            j_from_cmdline = 1;
        } else {
            global_flags[global_flag_count++] = argv[i];
        }
    }
    parse_umkfile("UMK");
    if (!j_from_cmdline) { if (parallel_auto) parallel_jobs = get_cpu_count(); }
    if (parallel_jobs < 1) parallel_jobs = 1;
    int ret;
    if (parallel_jobs > 1) ret = execute_parallel(argv[1]);
    else ret = execute_serial(argv[1]);
    free(global_flags);
    free_all();
    return ret;
}
