#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_LINE 4096
#define MAX_NAME 256

#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_RESET   "\033[0m"

/* ---------- Dynamic string array helpers ---------- */
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

/* ---------- Globals ---------- */
Rule *rules = NULL;
Command *commands = NULL;
Variable *variables = NULL;
char **global_flags = NULL;
int global_flag_count = 0;
int use_color = 1;
int dry_run = 0;

/* ---------- Forward declarations ---------- */
void trim(char *str);
int is_blank(const char *str);
char *expand(const char *str);
void add_variable(const char *name, const char *value);
char *get_variable(const char *name);
void parse_umkfile(const char *filename);
int execute(const char *target);
int execute_shell(const char *cmd);
int needs_rebuild(const char *target, StrVec *deps);
time_t get_mtime(const char *path);
int match_pattern(const char *name, const char *pattern);
char *apply_pattern(const char *target, const char *pattern);
char *wildcard(const char *pattern);
char *shell_cmd(const char *cmd);
void print_color(const char *color, const char *msg);
void free_all(void);
static int exec_command_list(StrVec *cmds);
static Flag *parse_flag_line(const char *line);

/* ---------- Utility functions ---------- */
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
    while (isspace(*s)) s++;
    if (*s == 0) { str[0] = 0; return; }
    char *e = s + strlen(s) - 1;
    while (e > s && isspace(*e)) e--;
    memmove(str, s, e - s + 1);
    str[e - s + 1] = 0;
}

int is_blank(const char *str) {
    while (*str) if (!isspace(*str++)) return 0;
    return 1;
}

void print_color(const char *color, const char *msg) {
    if (use_color && isatty(2))
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

/* ---------- Parsing helpers ---------- */
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

    char *start = strchr(copy, '(') + 1;
    char *end = strchr(start, ')');
    int len = end - start;
    strncpy(f->name, start, len);
    f->name[len] = 0;
    return f;
}

/* ---------- Parse UMK file ---------- */
void parse_umkfile(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        print_color(COLOR_RED, "No UMK file");
        exit(1);
    }

    char line[MAX_LINE];
    Command *cur_cmd = NULL;
    Flag *cur_flag = NULL;
    int in_flags = 0;

    while (fgets(line, sizeof(line), fp)) {
        char orig[MAX_LINE];
        strcpy(orig, line);
        trim(line);
        if (is_blank(line)) continue;

        // Variable assignment
        char *eq = strchr(line, '=');
        if (eq && !in_flags && !cur_cmd) {
            *eq = 0;
            char *name = line, *val = eq + 1;
            trim(name); trim(val);
            add_variable(name, expand(val));
            continue;
        }

        // Target definition
        char *colon = strchr(line, ':');
        if (colon && !in_flags && !cur_cmd) {
            *colon = 0;
            char *target = line;
            char *deps = colon + 1;
            trim(target);
            trim(deps);

            // Check if it's a command (no deps) or a rule
            if (strlen(deps) == 0) {
                // Command
                Command *cmd = malloc(sizeof(Command));
                strcpy(cmd->name, target);
                strvec_init(&cmd->commands);
                cmd->flags = NULL;
                cmd->next = commands;
                commands = cmd;
                cur_cmd = cmd;
            } else {
                // Rule
                Rule *r = malloc(sizeof(Rule));
                r->target = strdup(target);
                strvec_init(&r->deps);
                strvec_init(&r->commands);

                char *exp_deps = expand(deps);
                char *copy = strdup(exp_deps);
                char *tok = strtok(copy, " ");
                while (tok) {
                    strvec_add(&r->deps, tok);
                    tok = strtok(NULL, " ");
                }
                free(copy);

                // Read rule commands until "eoc"
                if (fgets(line, sizeof(line), fp)) {
                    char trimmed[MAX_LINE];
                    strcpy(trimmed, line);
                    trim(trimmed);
                    if (line[0] == '\t')
                        strvec_add(&r->commands, trimmed);
                }
                while (fgets(line, sizeof(line), fp)) {
                    char trimmed[MAX_LINE];
                    strcpy(trimmed, line);
                    trim(trimmed);
                    if (strcmp(trimmed, "eoc") == 0) break;
                    if (line[0] == '\t')
                        strvec_add(&r->commands, trimmed);
                }
                r->next = rules;
                rules = r;
            }
            continue;
        }

        if (strcmp(line, "eoc") == 0) {
            cur_cmd = NULL;
            in_flags = 0;
            cur_flag = NULL;
            continue;
        }

        if (!cur_cmd) continue;

        // Flags section
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

            if (cur_flag)
                strvec_add(&cur_flag->commands, line);
            continue;
        }

        // Regular command line
        if (line[0] != '\t')
            strvec_add(&cur_cmd->commands, line);
    }
    fclose(fp);
}

/* ---------- Execution helpers ---------- */
static int exec_command_list(StrVec *cmds) {
    for (int i = 0; i < cmds->count; i++) {
        char *exp = expand(cmds->items[i]);
        char line[MAX_LINE];
        strcpy(line, exp);
        trim(line);

        int ret;
        if (strncmp(line, "call ", 5) == 0) {
            char *target = line + 5;
            trim(target);
            ret = execute(target);
        } else {
            ret = execute_shell(line);
        }
        if (ret != 0) return ret;
    }
    return 0;
}

int execute_shell(const char *cmd) {
    if (dry_run) {
        printf("%s\n", cmd);
        return 0;
    }
    printf("%s\n", cmd);
    fflush(stdout);
    int ret = system(cmd);
    if (ret != 0) {
        char msg[MAX_LINE];
        snprintf(msg, sizeof(msg), "Error: %s (exit code: %d)", cmd, ret);
        print_color(COLOR_RED, msg);
    }
    return ret;
}

static int exec_flags_of_type(Command *c, int type) {
    for (Flag *f = c->flags; f; f = f->next) {
        if (f->type != type) continue;
        for (int i = 0; i < global_flag_count; i++) {
            char *flag_name = global_flags[i];
            if (flag_name[0] == '-') {
                flag_name += (flag_name[1] == '-') ? 2 : 1;
            }
            if (strcmp(f->name, flag_name) == 0) {
                int ret = exec_command_list(&f->commands);
                if (ret != 0) return ret;
                break;
            }
        }
    }
    return 0;
}

/* ---------- Main execution ---------- */
int execute(const char *target_name) {
    char clean[MAX_LINE];
    strcpy(clean, target_name);
    trim(clean);
    if (strlen(clean) == 0) return 0;

    // Try as a command
    for (Command *c = commands; c; c = c->next) {
        if (strcmp(c->name, clean) != 0) continue;

        int ret = exec_flags_of_type(c, 0); // BEFORE flags
        if (ret != 0) return ret;

        ret = exec_command_list(&c->commands);
        if (ret != 0) return ret;

        ret = exec_flags_of_type(c, 1); // AFTER flags
        return ret;
    }

    // Try as a rule
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
                    int res_len = 0;
                    for (char *p = buf; *p; p++) {
                        if (*p == '%') {
                            strcpy(res + res_len, stem);
                            res_len += strlen(stem);
                        } else {
                            res[res_len++] = *p;
                            res[res_len] = '\0';
                        }
                    }
                    strvec_add(&actual_deps, res);
                }
            }
        } else {
            for (int i = 0; i < r->deps.count; i++)
                strvec_add(&actual_deps, r->deps.items[i]);
        }

        // Build dependencies
        for (int i = 0; i < actual_deps.count; i++) {
            int ret = execute(actual_deps.items[i]);
            if (ret != 0) {
                strvec_free(&actual_deps);
                return ret;
            }
        }

        if (!needs_rebuild(clean, &actual_deps) && !dry_run) {
            strvec_free(&actual_deps);
            return 0;
        }

        // Execute rule commands with automatic variables
        for (int i = 0; i < r->commands.count; i++) {
            char res[MAX_LINE] = "";
            int res_len = 0;
            char *cmd = r->commands.items[i];
            for (char *p = cmd; *p; p++) {
                if (*p == '$' && p[1] == '@') {
                    strcpy(res + res_len, clean);
                    res_len += strlen(clean);
                    p++;
                } else if (*p == '$' && p[1] == '<') {
                    if (actual_deps.count > 0) {
                        strcpy(res + res_len, actual_deps.items[0]);
                        res_len += strlen(actual_deps.items[0]);
                    }
                    p++;
                } else if (*p == '$' && p[1] == '^') {
                    for (int j = 0; j < actual_deps.count; j++) {
                        if (j > 0) {
                            res[res_len++] = ' ';
                            res[res_len] = '\0';
                        }
                        strcpy(res + res_len, actual_deps.items[j]);
                        res_len += strlen(actual_deps.items[j]);
                    }
                    p++;
                } else {
                    res[res_len++] = *p;
                    res[res_len] = '\0';
                }
            }
            char *exp = expand(res);
            int ret = execute_shell(exp);
            if (ret != 0) {
                strvec_free(&actual_deps);
                return ret;
            }
        }

        strvec_free(&actual_deps);
        return 0;
    }

    // Target exists as a file
    if (get_mtime(clean) != 0) return 0;

    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), "Unknown target: %s", clean);
    print_color(COLOR_RED, msg);
    return 1;
}

/* ---------- Cleanup ---------- */
void free_all(void) {
    Rule *r = rules;
    while (r) {
        Rule *next = r->next;
        free(r->target);
        strvec_free(&r->deps);
        strvec_free(&r->commands);
        free(r);
        r = next;
    }

    Command *c = commands;
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

    Variable *v = variables;
    while (v) {
        Variable *next = v->next;
        free(v->value);
        free(v);
        v = next;
    }
}

/* ---------- Main ---------- */
int main(int argc, char **argv) {
    if (argc < 2) {
        print_color(COLOR_YELLOW, "Usage: umk <command> [flags...]");
        return 1;
    }

    global_flag_count = argc - 2;
    global_flags = malloc(global_flag_count * sizeof(char*));
    for (int i = 0; i < global_flag_count; i++)
        global_flags[i] = argv[i + 2];

    parse_umkfile("UMK");

    int ret = execute(argv[1]);

    free(global_flags);
    free_all();
    return ret;
}
