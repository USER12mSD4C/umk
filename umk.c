#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define MAX_LINE 4096
#define MAX_NAME 256

#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_RESET   "\033[0m"

typedef struct Flag {
    char name[MAX_NAME];
    int type;
    char **commands;
    int cmd_count;
    struct Flag *next;
} Flag;

typedef struct Rule {
    char *target;
    char **deps;
    int dep_cnt;
    char **commands;
    int cmd_cnt;
    struct Rule *next;
} Rule;

typedef struct Command {
    char name[MAX_NAME];
    char **commands;
    int cmd_count;
    Flag *flags;
    struct Command *next;
} Command;

typedef struct Variable {
    char name[MAX_NAME];
    char *value;
    struct Variable *next;
} Variable;

Rule *rules = NULL;
Command *commands = NULL;
Variable *variables = NULL;
char **global_flags = NULL;
int global_flag_count = 0;
int use_color = 1;
int dry_run = 0;

void trim(char *str);
int is_blank(const char *str);
char *expand(const char *str);
void add_variable(const char *name, const char *value);
char *get_variable(const char *name);
void add_rule(const char *target, const char *deps, const char *cmd);
void add_command(const char *name);
void add_command_line(Command *cmd, const char *line);
void add_flag(Command *cmd, const char *flag_line);
void add_flag_command(Flag *flag, const char *line);
void parse_umkfile(const char *filename);
int execute(const char *target);
int execute_shell(const char *cmd);
int needs_rebuild(const char *target, char **deps, int dep_cnt);
time_t get_mtime(const char *path);
int match_pattern(const char *name, const char *pattern);
char *apply_pattern(const char *target, const char *pattern);
char **split(const char *str, int *cnt);
char *wildcard(const char *pattern);
char *shell_cmd(const char *cmd);
void print_color(const char *color, const char *msg);
void free_all(void);

time_t get_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return st.st_mtime;
    return 0;
}

int needs_rebuild(const char *target, char **deps, int dep_cnt) {
    time_t t = get_mtime(target);
    if (t == 0) return 1;
    for (int i = 0; i < dep_cnt; i++) {
        time_t d = get_mtime(deps[i]);
        if (d == 0) return 1;
        if (d > t) return 1;
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
    if (use_color && isatty(2)) fprintf(stderr, "%s%s%s\n", color, msg, COLOR_RESET);
    else fprintf(stderr, "%s\n", msg);
}

void add_variable(const char *name, const char *value) {
    Variable *v = variables;
    while (v) {
        if (strcmp(v->name, name) == 0) {
            free(v->value);
            v->value = strdup(value);
            return;
        }
        v = v->next;
    }
    Variable *nv = malloc(sizeof(Variable));
    strcpy(nv->name, name);
    nv->value = strdup(value);
    nv->next = variables;
    variables = nv;
}

char *get_variable(const char *name) {
    Variable *v = variables;
    while (v) {
        if (strcmp(v->name, name) == 0) return v->value;
        v = v->next;
    }
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
    if (suf > 0) {
        const char *s = p + pre + 1;
        if (strcmp(target + tlen - suf, s) != 0) return NULL;
    }
    int stem_len = tlen - pre - suf;
    strncpy(res, target + pre, stem_len);
    res[stem_len] = 0;
    return res;
}

char **split(const char *str, int *cnt) {
    char *copy = strdup(str);
    char **res = NULL;
    *cnt = 0;
    char *tok = strtok(copy, " ");
    while (tok) {
        res = realloc(res, (*cnt + 1) * sizeof(char*));
        res[*cnt] = strdup(tok);
        (*cnt)++;
        tok = strtok(NULL, " ");
    }
    free(copy);
    return res;
}

char *expand(const char *str) {
    static char res[MAX_LINE];
    res[0] = 0;
    const char *p = str;
    while (*p) {
        if (*p == '$' && p[1] == '(') {
            const char *end = strchr(p, ')');
            if (!end) { strncat(res, p, MAX_LINE - strlen(res) - 1); break; }
            char *inner = malloc(end - p - 1);
            strncpy(inner, p + 2, end - p - 2);
            inner[end - p - 2] = 0;
            char *space = strchr(inner, ' ');
            if (space) {
                *space = 0;
                char *func = inner;
                char *args = space + 1;
                int ac = 1;
                for (char *q = args; *q; q++) if (*q == ' ') ac++;
                const char **av = malloc(ac * sizeof(char*));
                char *arg = strtok(args, " ");
                int i = 0;
                while (arg) { av[i++] = arg; arg = strtok(NULL, " "); }
                char *val = "";
                if (strcmp(func, "wildcard") == 0) val = wildcard(av[0]);
                else if (strcmp(func, "shell") == 0) val = shell_cmd(av[0]);
                strcat(res, val);
                free(av);
            } else {
                char *val = get_variable(inner);
                if (val) strcat(res, val);
            }
            free(inner);
            p = end + 1;
        } else {
            char ch[2] = {*p, 0};
            strcat(res, ch);
            p++;
        }
    }
    return res;
}

void add_rule(const char *target, const char *deps, const char *cmd) {
    Rule *r = malloc(sizeof(Rule));
    r->target = strdup(target);
    r->deps = split(deps, &r->dep_cnt);
    r->commands = malloc(sizeof(char*));
    r->commands[0] = strdup(cmd);
    r->cmd_cnt = 1;
    r->next = rules;
    rules = r;
}

void add_command(const char *name) {
    Command *c = malloc(sizeof(Command));
    strcpy(c->name, name);
    c->commands = NULL;
    c->cmd_count = 0;
    c->flags = NULL;
    c->next = commands;
    commands = c;
}

void add_command_line(Command *cmd, const char *line) {
    cmd->commands = realloc(cmd->commands, (cmd->cmd_count + 1) * sizeof(char*));
    cmd->commands[cmd->cmd_count] = strdup(line);
    cmd->cmd_count++;
}

void add_flag(Command *cmd, const char *flag_line) {
    Flag *f = malloc(sizeof(Flag));
    f->commands = NULL;
    f->cmd_count = 0;
    f->next = cmd->flags;
    cmd->flags = f;
    char copy[MAX_LINE];
    strcpy(copy, flag_line);
    trim(copy);
    f->type = (copy[0] == '-') ? 0 : 1;
    char *start = strchr(copy, '(') + 1;
    char *end = strchr(start, ')');
    int len = end - start;
    strncpy(f->name, start, len);
    f->name[len] = 0;
}

void add_flag_command(Flag *flag, const char *line) {
    flag->commands = realloc(flag->commands, (flag->cmd_count + 1) * sizeof(char*));
    flag->commands[flag->cmd_count] = strdup(line);
    flag->cmd_count++;
}

void parse_umkfile(const char *file) {
    FILE *fp = fopen(file, "r");
    if (!fp) { print_color(COLOR_RED, "No UMK file"); exit(1); }
    
    char line[MAX_LINE];
    Command *cur_cmd = NULL;
    Flag *cur_flag = NULL;
    int in_flags = 0, in_flag = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        char orig[MAX_LINE];
        strcpy(orig, line);
        trim(line);
        if (is_blank(line)) continue;
        
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
            char *target = line;
            char *deps = colon + 1;
            trim(target);
            trim(deps);
            
            long pos = ftell(fp);
            char next_line[MAX_LINE];
            
            if (fgets(next_line, sizeof(next_line), fp)) {
                char trimmed_next[MAX_LINE];
                strcpy(trimmed_next, next_line);
                trim(trimmed_next);
                
                // Если есть зависимости — это правило
                if (strlen(deps) > 0) {
                    Rule *r = malloc(sizeof(Rule));
                    r->target = strdup(target);
                    char *expanded_deps = expand(deps);
                    r->deps = split(expanded_deps, &r->dep_cnt);
                    r->commands = NULL;
                    r->cmd_cnt = 0;
                    r->next = rules;
                    rules = r;
                    
                    if (next_line[0] == '\t') {
                        r->commands = realloc(r->commands, sizeof(char*));
                        r->commands[0] = strdup(trimmed_next);
                        r->cmd_cnt = 1;
                    }
                    
                    while (fgets(line, sizeof(line), fp)) {
                        char line_copy[MAX_LINE];
                        strcpy(line_copy, line);
                        trim(line_copy);
                        if (strcmp(line_copy, "eoc") == 0) break;
                        if (line[0] == '\t') {
                            trim(line_copy);
                            r->commands = realloc(r->commands, (r->cmd_cnt + 1) * sizeof(char*));
                            r->commands[r->cmd_cnt] = strdup(line_copy);
                            r->cmd_cnt++;
                        }
                    }
                } else {
                    add_command(target);
                    cur_cmd = commands;
                    fseek(fp, pos, SEEK_SET);
                }
            } else {
                add_command(target);
            }
            continue;
        }
        
        if (strcmp(line, "eoc") == 0) {
            cur_cmd = NULL;
            in_flags = 0;
            in_flag = 0;
            continue;
        }
        
        if (!cur_cmd) continue;
        
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
            
            char *trimmed = orig;
            while (isspace(*trimmed)) trimmed++;
            if (strncmp(trimmed, "-fg(", 4) == 0 || strncmp(trimmed, "+fg(", 4) == 0) {
                Flag *f = malloc(sizeof(Flag));
                f->commands = NULL;
                f->cmd_count = 0;
                f->next = cur_cmd->flags;
                cur_cmd->flags = f;
                
                char copy[MAX_LINE];
                strcpy(copy, trimmed);
                trim(copy);
                f->type = (copy[0] == '-') ? 0 : 1;
                
                char *start = strchr(copy, '(') + 1;
                char *end = strchr(start, ')');
                int len = end - start;
                strncpy(f->name, start, len);
                f->name[len] = 0;
                
                cur_flag = f;
                in_flag = 1;
                continue;
            }
            
            if (strcmp(line, "eofg") == 0) {
                cur_flag = NULL;
                in_flag = 0;
                continue;
            }
            
            if (in_flag && cur_flag) {
                cur_flag->commands = realloc(cur_flag->commands, (cur_flag->cmd_count + 1) * sizeof(char*));
                cur_flag->commands[cur_flag->cmd_count] = strdup(line);
                cur_flag->cmd_count++;
                continue;
            }
        }
        
        if (!in_flags && line[0] != '\t') {
            add_command_line(cur_cmd, line);
        }
    }
    fclose(fp);
}

int execute_shell(const char *cmd) {
    if (dry_run) { printf("%s\n", cmd); return 0; }
    printf("%s\n", cmd);
    fflush(stdout);
    int ret = system(cmd);
    if (ret != 0) {
        char msg[MAX_LINE];
        snprintf(msg, sizeof(msg), "Error: %s (exit code: %d)", cmd, ret);
        print_color(COLOR_RED, msg);
        return ret;
    }
    return 0;
}

int execute(const char *target_name) {
    char clean[MAX_LINE];
    strcpy(clean, target_name);
    trim(clean);
    if (strlen(clean) == 0) return 0;
    
    // Ищем команду
    Command *c = commands;
    while (c) {
        if (strcmp(c->name, clean) == 0) break;
        c = c->next;
    }
    
    if (c) {
        // BEFORE flags
        Flag *f = c->flags;
        while (f) {
            for (int i = 0; i < global_flag_count; i++) {
                char *flag_name = global_flags[i];
                if (flag_name[0] == '-') {
                    if (flag_name[1] == '-') flag_name += 2;
                    else flag_name += 1;
                }
                if (strcmp(f->name, flag_name) == 0 && f->type == 0) {
                    for (int j = 0; j < f->cmd_count; j++) {
                        char *exp = expand(f->commands[j]);
                        char line[MAX_LINE];
                        strcpy(line, exp);
                        trim(line);
                        if (strncmp(line, "call ", 5) == 0) {
                            char *target = line + 5;
                            trim(target);
                            int ret = execute(target);
                            if (ret != 0) return ret;
                        } else {
                            int ret = execute_shell(line);
                            if (ret != 0) return ret;
                        }
                    }
                }
            }
            f = f->next;
        }
        
        // Основные команды
        for (int i = 0; i < c->cmd_count; i++) {
            char *exp = expand(c->commands[i]);
            char line[MAX_LINE];
            strcpy(line, exp);
            trim(line);
            if (strncmp(line, "call ", 5) == 0) {
                char *target = line + 5;
                trim(target);
                int ret = execute(target);
                if (ret != 0) return ret;
            } else {
                int ret = execute_shell(line);
                if (ret != 0) return ret;
            }
        }
        
        // AFTER flags
        f = c->flags;
        while (f) {
            for (int i = 0; i < global_flag_count; i++) {
                char *flag_name = global_flags[i];
                if (flag_name[0] == '-') {
                    if (flag_name[1] == '-') flag_name += 2;
                    else flag_name += 1;
                }
                if (strcmp(f->name, flag_name) == 0 && f->type == 1) {
                    for (int j = 0; j < f->cmd_count; j++) {
                        char *exp = expand(f->commands[j]);
                        char line[MAX_LINE];
                        strcpy(line, exp);
                        trim(line);
                        if (strncmp(line, "call ", 5) == 0) {
                            char *target = line + 5;
                            trim(target);
                            int ret = execute(target);
                            if (ret != 0) return ret;
                        } else {
                            int ret = execute_shell(line);
                            if (ret != 0) return ret;
                        }
                    }
                }
            }
            f = f->next;
        }
        return 0;
    }
    
    // Ищем правило
    Rule *r = rules;
    while (r) {
        if (match_pattern(clean, r->target)) break;
        r = r->next;
    }
    
    if (r) {
        char **deps_exp = NULL;
        int dep_cnt = 0;
        
        if (strchr(r->target, '%')) {
            char *stem = apply_pattern(clean, r->target);
            if (stem) {
                for (int i = 0; i < r->dep_cnt; i++) {
                    char buf[MAX_LINE];
                    strcpy(buf, r->deps[i]);
                    char res[MAX_LINE];
                    res[0] = 0;
                    char *p = buf;
                    while (*p) {
                        if (*p == '%') {
                            strcat(res, stem);
                            p++;
                        } else {
                            char ch[2] = {*p, 0};
                            strcat(res, ch);
                            p++;
                        }
                    }
                    deps_exp = realloc(deps_exp, (dep_cnt + 1) * sizeof(char*));
                    deps_exp[dep_cnt] = strdup(res);
                    dep_cnt++;
                }
            }
        } else {
            deps_exp = r->deps;
            dep_cnt = r->dep_cnt;
        }
        
        for (int i = 0; i < dep_cnt; i++) {
            int ret = execute(deps_exp[i]);
            if (ret != 0) return ret;
        }
        
        if (!needs_rebuild(clean, deps_exp, dep_cnt) && dry_run == 0) {
            if (deps_exp != r->deps) {
                for (int i = 0; i < dep_cnt; i++) free(deps_exp[i]);
                free(deps_exp);
            }
            return 0;
        }
        
        for (int i = 0; i < r->cmd_cnt; i++) {
            char cmd_buf[MAX_LINE];
            strcpy(cmd_buf, r->commands[i]);
            char res[MAX_LINE];
            res[0] = 0;
            char *p = cmd_buf;
            while (*p) {
                if (*p == '$' && p[1] == '@') {
                    strcat(res, clean);
                    p += 2;
                } else if (*p == '$' && p[1] == '<') {
                    if (dep_cnt > 0) strcat(res, deps_exp[0]);
                    p += 2;
                } else if (*p == '$' && p[1] == '^') {
                    for (int j = 0; j < dep_cnt; j++) {
                        if (j > 0) strcat(res, " ");
                        strcat(res, deps_exp[j]);
                    }
                    p += 2;
                } else {
                    char ch[2] = {*p, 0};
                    strcat(res, ch);
                    p++;
                }
            }
            char *exp = expand(res);
            int ret = execute_shell(exp);
            if (ret != 0) {
                if (deps_exp != r->deps) {
                    for (int j = 0; j < dep_cnt; j++) free(deps_exp[j]);
                    free(deps_exp);
                }
                return ret;
            }
        }
        
        if (deps_exp != r->deps) {
            for (int i = 0; i < dep_cnt; i++) free(deps_exp[i]);
            free(deps_exp);
        }
        return 0;
    }
    
    if (get_mtime(clean) != 0) return 0;
    
    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), "Unknown target: %s", clean);
    print_color(COLOR_RED, msg);
    return 1;
}

void free_all(void) {
    Rule *r = rules;
    while (r) {
        Rule *next = r->next;
        free(r->target);
        for (int i = 0; i < r->dep_cnt; i++) free(r->deps[i]);
        free(r->deps);
        for (int i = 0; i < r->cmd_cnt; i++) free(r->commands[i]);
        free(r->commands);
        free(r);
        r = next;
    }
    
    Command *c = commands;
    while (c) {
        Command *next = c->next;
        for (int i = 0; i < c->cmd_count; i++) free(c->commands[i]);
        free(c->commands);
        Flag *f = c->flags;
        while (f) {
            Flag *nextf = f->next;
            for (int i = 0; i < f->cmd_count; i++) free(f->commands[i]);
            free(f->commands);
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

int main(int argc, char **argv) {
    if (argc < 2) {
        print_color(COLOR_YELLOW, "Usage: umk <command> [flags...]");
        return 1;
    }
    
    global_flag_count = argc - 2;
    global_flags = malloc(global_flag_count * sizeof(char*));
    for (int i = 0; i < global_flag_count; i++) {
        global_flags[i] = argv[i + 2];
    }
    
    parse_umkfile("UMK");
    
    int ret = execute(argv[1]);
    
    free(global_flags);
    free_all();
    return ret;
}
