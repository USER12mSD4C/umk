// umk.c - полноценная замена make
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
#define MAX_JOBS 64
#define MAX_FILES 1024

#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_RESET   "\033[0m"

typedef struct ExprNode {
    int type;
    char *value;
    char *func_name;
    struct ExprNode **args;
    int arg_count;
} ExprNode;

typedef struct PatternRule {
    char *target_pattern;
    char *dep_pattern;
    char **commands;
    int cmd_count;
    struct PatternRule *next;
} PatternRule;

typedef struct Flag {
    char name[MAX_NAME];
    int type;
    char **commands;
    int cmd_count;
    struct Flag *next;
} Flag;

typedef struct Command {
    char name[MAX_NAME];
    char **main_commands;
    int main_cmd_count;
    Flag *flags;
    struct Command *next;
    int is_phony;
    char **deps;
    int dep_count;
    int is_rule;
} Command;

typedef struct Variable {
    char name[MAX_NAME];
    char *value;
    struct Variable *next;
} Variable;

typedef struct Job {
    char *command;
    pid_t pid;
    int ret;
    struct Job *next;
} Job;

Command *commands = NULL;
Variable *variables = NULL;
PatternRule *pattern_rules = NULL;
int use_color = 1;
int jobs = 1;
int dry_run = 0;
Job *job_queue = NULL;

// Прототипы
void trim(char *str);
int is_blank(const char *str);
char *expand_string(const char *str);
char *expand_string_with_special(const char *str, const char *target, const char *dep, const char *all_deps);
void add_variable(const char *name, const char *value);
char *get_variable(const char *name);
void set_special_vars(const char *target, const char *dep, const char *all_deps);
void add_command(const char *name, int is_rule);
void add_main_command(Command *cmd, const char *line);
void add_dependencies(Command *cmd, const char *dep_str);
void add_flag(Command *cmd, const char *flag_line);
void add_flag_command(Flag *flag, const char *line);
void add_pattern_rule(const char *target, const char *dep, const char *cmd);
void parse_umkfile(const char *filename);
int evaluate_condition(const char *expr);
int execute_target(const char *target_name);
int execute_commands(char **cmds, int cmd_count);
int execute_single_command(const char *cmd);
int execute_flag(Flag *flag);
int execute_pattern_rule(const char *target, PatternRule *rule);
int needs_rebuild(const char *target, char **deps, int dep_count);
time_t get_mtime(const char *path);
int match_pattern(const char *name, const char *pattern);
char *apply_pattern(const char *target, const char *pattern);
char *expand_dep_pattern(const char *pattern, const char *stem);
char **split_deps(const char *dep_str, int *count);
void run_jobs_parallel(void);
void add_job(const char *cmd);
void clear_jobs(void);
char *wildcard(const char *pattern);
char *shell(const char *cmd);
void print_color(const char *color, const char *msg);
PatternRule *find_pattern_rule(const char *target);
char **expand_variable_list(const char *var_name, int *count);
void collect_source_files(const char *dir, char ***files, int *count);

// Встроенные функции
typedef struct {
    char *name;
    char *(*func)(const char **args, int arg_count);
} BuiltinFunc;

char *func_wildcard(const char **args, int arg_count);
char *func_shell(const char **args, int arg_count);

BuiltinFunc builtins[] = {
    {"wildcard", func_wildcard},
    {"shell", func_shell},
    {NULL, NULL}
};

// ==== Реализация ====

time_t get_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return st.st_mtime;
    return 0;
}

int needs_rebuild(const char *target, char **deps, int dep_count) {
    time_t target_time = get_mtime(target);
    if (target_time == 0) return 1;
    for (int i = 0; i < dep_count; i++) {
        time_t dep_time = get_mtime(deps[i]);
        if (dep_time == 0) return 1;
        if (dep_time > target_time) return 1;
    }
    return 0;
}

void trim(char *str) {
    char *start = str;
    char *end;
    while (isspace((unsigned char)*start)) start++;
    if (*start == 0) { str[0] = '\0'; return; }
    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    memmove(str, start, end - start + 1);
    str[end - start + 1] = '\0';
}

int is_blank(const char *str) {
    while (*str) if (!isspace((unsigned char)*str++)) return 0;
    return 1;
}

void print_color(const char *color, const char *msg) {
    if (use_color && isatty(STDERR_FILENO)) {
        fprintf(stderr, "%s%s%s\n", color, msg, COLOR_RESET);
    } else {
        fprintf(stderr, "%s\n", msg);
    }
}

void add_variable(const char *name, const char *value) {
    Variable *var = variables;
    while (var) {
        if (strcmp(var->name, name) == 0) {
            free(var->value);
            var->value = strdup(value);
            return;
        }
        var = var->next;
    }
    Variable *new_var = malloc(sizeof(Variable));
    strcpy(new_var->name, name);
    new_var->value = strdup(value);
    new_var->next = variables;
    variables = new_var;
}

char *get_variable(const char *name) {
    Variable *var = variables;
    while (var) {
        if (strcmp(var->name, name) == 0) return var->value;
        var = var->next;
    }
    return NULL;
}

void set_special_vars(const char *target, const char *dep, const char *all_deps) {
    if (target) add_variable("@", target);
    if (dep) add_variable("<", dep);
    if (all_deps) add_variable("^", all_deps);
}

int match_pattern(const char *name, const char *pattern) {
    const char *p = pattern, *n = name;
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return 1;
            while (*n) if (match_pattern(n++, p)) return 1;
            return 0;
        } else if (*p != *n) return 0;
        p++; n++;
    }
    return *n == '\0';
}

char *apply_pattern(const char *target, const char *pattern) {
    static char result[MAX_LINE];
    char *star_pos = (char*)strchr(pattern, '*');
    if (!star_pos) { strcpy(result, pattern); return result; }
    
    int prefix_len = star_pos - pattern;
    int suffix_len = strlen(pattern) - (prefix_len + 1);
    if (strncmp(target, pattern, prefix_len) != 0) return NULL;
    
    int target_len = strlen(target);
    if (target_len < prefix_len + suffix_len) return NULL;
    
    if (suffix_len > 0) {
        const char *suffix = pattern + prefix_len + 1;
        if (strcmp(target + target_len - suffix_len, suffix) != 0) return NULL;
    }
    
    int stem_len = target_len - prefix_len - suffix_len;
    strncpy(result, target + prefix_len, stem_len);
    result[stem_len] = '\0';
    return result;
}

char *expand_dep_pattern(const char *pattern, const char *stem) {
    static char result[MAX_LINE];
    char *p = (char*)pattern;
    char *out = result;
    while (*p) {
        if (*p == '*' && (p == pattern || *(p-1) != '$')) {
            strcpy(out, stem);
            out += strlen(stem);
            p++;
        } else if (*p == '$' && *(p+1) == '*') {
            strcpy(out, stem);
            out += strlen(stem);
            p += 2;
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    return result;
}

char **split_deps(const char *dep_str, int *count) {
    char *copy = strdup(dep_str);
    char **result = NULL;
    *count = 0;
    char *token = strtok(copy, " ");
    while (token) {
        result = realloc(result, (*count + 1) * sizeof(char*));
        result[*count] = strdup(token);
        (*count)++;
        token = strtok(NULL, " ");
    }
    free(copy);
    return result;
}

char *wildcard(const char *pattern) {
    static char result[MAX_LINE];
    result[0] = '\0';
    DIR *dir = opendir(".");
    if (!dir) return result;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (match_pattern(entry->d_name, pattern)) {
            if (strlen(result) > 0) strcat(result, " ");
            strcat(result, entry->d_name);
        }
    }
    closedir(dir);
    return result;
}

char *shell(const char *cmd) {
    static char result[MAX_LINE];
    result[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) return result;
    char line[MAX_LINE];
    if (fgets(line, sizeof(line), fp)) {
        trim(line);
        strcpy(result, line);
    }
    pclose(fp);
    return result;
}

char *func_wildcard(const char **args, int arg_count) {
    return arg_count > 0 ? wildcard(args[0]) : "";
}

char *func_shell(const char **args, int arg_count) {
    return arg_count > 0 ? shell(args[0]) : "";
}

char *expand_string(const char *str) {
    static char result[MAX_LINE];
    result[0] = '\0';
    
    const char *p = str;
    while (*p) {
        if (*p == '$' && *(p+1) == '(') {
            const char *end = strchr(p, ')');
            if (!end) { strncat(result, p, MAX_LINE - strlen(result) - 1); break; }
            
            char *inner = malloc(end - p - 1);
            strncpy(inner, p + 2, end - p - 2);
            inner[end - p - 2] = '\0';
            
            char *space = strchr(inner, ' ');
            if (space) {
                *space = '\0';
                char *func_name = inner;
                char *args_str = space + 1;
                
                int arg_count = 1;
                for (char *q = args_str; *q; q++) if (*q == ' ') arg_count++;
                
                const char **args = malloc(arg_count * sizeof(char*));
                char *arg = strtok(args_str, " ");
                int i = 0;
                while (arg) {
                    args[i++] = arg;
                    arg = strtok(NULL, " ");
                }
                
                char *val = "";
                for (int j = 0; builtins[j].name; j++) {
                    if (strcmp(func_name, builtins[j].name) == 0) {
                        val = builtins[j].func(args, arg_count);
                        break;
                    }
                }
                strcat(result, val);
                free(args);
            } else {
                char *val = get_variable(inner);
                if (val) strcat(result, val);
            }
            free(inner);
            p = end + 1;
        } else {
            char ch[2] = {*p, '\0'};
            strcat(result, ch);
            p++;
        }
    }
    return result;
}

char *expand_string_with_special(const char *str, const char *target, const char *dep, const char *all_deps) {
    static char result[MAX_LINE];
    result[0] = '\0';
    
    const char *p = str;
    while (*p) {
        if (*p == '$' && *(p+1) == '@') {
            strcat(result, target);
            p += 2;
        } else if (*p == '$' && *(p+1) == '<') {
            strcat(result, dep);
            p += 2;
        } else if (*p == '$' && *(p+1) == '^') {
            strcat(result, all_deps);
            p += 2;
        } else if (*p == '$' && *(p+1) == '(') {
            const char *end = strchr(p, ')');
            if (!end) { strncat(result, p, MAX_LINE - strlen(result) - 1); break; }
            
            char *inner = malloc(end - p - 1);
            strncpy(inner, p + 2, end - p - 2);
            inner[end - p - 2] = '\0';
            
            char *space = strchr(inner, ' ');
            if (space) {
                *space = '\0';
                char *func_name = inner;
                char *args_str = space + 1;
                
                int arg_count = 1;
                for (char *q = args_str; *q; q++) if (*q == ' ') arg_count++;
                
                const char **args = malloc(arg_count * sizeof(char*));
                char *arg = strtok(args_str, " ");
                int i = 0;
                while (arg) {
                    args[i++] = arg;
                    arg = strtok(NULL, " ");
                }
                
                char *val = "";
                for (int j = 0; builtins[j].name; j++) {
                    if (strcmp(func_name, builtins[j].name) == 0) {
                        val = builtins[j].func(args, arg_count);
                        break;
                    }
                }
                strcat(result, val);
                free(args);
            } else {
                char *val = get_variable(inner);
                if (val) strcat(result, val);
            }
            free(inner);
            p = end + 1;
        } else {
            char ch[2] = {*p, '\0'};
            strcat(result, ch);
            p++;
        }
    }
    return result;
}

int evaluate_condition(const char *expr) {
    char expanded[MAX_LINE];
    strcpy(expanded, expand_string(expr));
    trim(expanded);
    
    char *eq = strchr(expanded, '=');
    if (eq && *(eq+1) == '=') {
        *eq = '\0';
        char *left = expanded;
        char *right = eq + 2;
        trim(left);
        trim(right);
        return strcmp(left, right) == 0;
    }
    return strlen(expanded) > 0 && strcmp(expanded, "0") != 0;
}

void add_command(const char *name, int is_rule) {
    Command *new_cmd = malloc(sizeof(Command));
    strcpy(new_cmd->name, name);
    new_cmd->main_commands = NULL;
    new_cmd->main_cmd_count = 0;
    new_cmd->flags = NULL;
    new_cmd->next = commands;
    new_cmd->is_phony = !is_rule;
    new_cmd->deps = NULL;
    new_cmd->dep_count = 0;
    new_cmd->is_rule = is_rule;
    commands = new_cmd;
}

Command *find_command(const char *name) {
    Command *cmd = commands;
    while (cmd) {
        if (strcmp(cmd->name, name) == 0) return cmd;
        cmd = cmd->next;
    }
    return NULL;
}

void add_main_command(Command *cmd, const char *line) {
    cmd->main_commands = realloc(cmd->main_commands, (cmd->main_cmd_count + 1) * sizeof(char*));
    cmd->main_commands[cmd->main_cmd_count] = strdup(line);
    cmd->main_cmd_count++;
}

void add_dependencies(Command *cmd, const char *dep_str) {
    char *expanded = expand_string(dep_str);
    cmd->deps = split_deps(expanded, &cmd->dep_count);
}

void add_flag(Command *cmd, const char *flag_line) {
    Flag *new_flag = malloc(sizeof(Flag));
    new_flag->commands = NULL;
    new_flag->cmd_count = 0;
    new_flag->next = cmd->flags;
    cmd->flags = new_flag;
    
    char line_copy[MAX_LINE];
    strcpy(line_copy, flag_line);
    trim(line_copy);
    
    if (strncmp(line_copy, "-fg(", 4) == 0) new_flag->type = 0;
    else new_flag->type = 1;
    
    char *start = strchr(line_copy, '(') + 1;
    char *end = strchr(start, ')');
    int len = end - start;
    strncpy(new_flag->name, start, len);
    new_flag->name[len] = '\0';
}

void add_flag_command(Flag *flag, const char *line) {
    flag->commands = realloc(flag->commands, (flag->cmd_count + 1) * sizeof(char*));
    flag->commands[flag->cmd_count] = strdup(line);
    flag->cmd_count++;
}

void add_pattern_rule(const char *target, const char *dep, const char *cmd) {
    PatternRule *rule = malloc(sizeof(PatternRule));
    rule->target_pattern = strdup(target);
    rule->dep_pattern = strdup(dep);
    rule->commands = malloc(sizeof(char*));
    rule->commands[0] = strdup(cmd);
    rule->cmd_count = 1;
    rule->next = pattern_rules;
    pattern_rules = rule;
}

PatternRule *find_pattern_rule(const char *target) {
    PatternRule *rule = pattern_rules;
    while (rule) {
        if (match_pattern(target, rule->target_pattern)) return rule;
        rule = rule->next;
    }
    return NULL;
}

void parse_umkfile(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) { print_color(COLOR_RED, "No UMK file found"); exit(1); }
    
    char line[MAX_LINE];
    Command *current_cmd = NULL;
    Flag *current_flag = NULL;
    int in_flags_block = 0, in_flag = 0;
    int in_condition = 0, condition_result = 1, skip_until_endif = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        char original[MAX_LINE];
        strcpy(original, line);
        trim(line);
        if (is_blank(line)) continue;
        
        if (strncmp(line, "if ", 3) == 0) {
            in_condition = 1;
            condition_result = evaluate_condition(line + 3);
            skip_until_endif = !condition_result;
            continue;
        }
        if (strcmp(line, "else") == 0) {
            if (in_condition) { skip_until_endif = condition_result; condition_result = !condition_result; }
            continue;
        }
        if (strcmp(line, "endif") == 0) { in_condition = 0; skip_until_endif = 0; condition_result = 1; continue; }
        if (skip_until_endif) continue;
        
        char *eq = strchr(line, '=');
        if (eq && !in_flags_block && !current_cmd && !in_condition) {
            *eq = '\0';
            char *var_name = line, *var_value = eq + 1;
            trim(var_name); trim(var_value);
            add_variable(var_name, expand_string(var_value));
            continue;
        }
        
        char *colon = strchr(line, ':');
        if (colon && strchr(line, '%') && !in_flags_block && !current_cmd) {
            *colon = '\0';
            char *target = line, *dep = colon + 1;
            trim(target); trim(dep);
            if (fgets(line, sizeof(line), fp)) {
                trim(line);
                if (line[0] == '\t' || line[0] == ' ') {
                    add_pattern_rule(target, dep, line + 1);
                }
            }
            continue;
        }
        
        if (strcmp(line, "eoc") == 0) { current_cmd = NULL; in_flags_block = 0; in_flag = 0; continue; }
        
        colon = strchr(line, ':');
        if (colon && !in_flags_block && strcmp(line, "+flags:") != 0) {
            int in_quotes = 0, is_command = 1;
            for (char *p = line; p < colon; p++) { if (*p == '"') in_quotes = !in_quotes; if (in_quotes) { is_command = 0; break; } }
            if (is_command) {
                *colon = '\0';
                add_command(line, 1);
                current_cmd = commands;
                char *deps_str = colon + 1;
                trim(deps_str);
                if (strlen(deps_str) > 0) {
                    add_dependencies(current_cmd, deps_str);
                }
                continue;
            }
        }
        
        if (!current_cmd) continue;
        if (strcmp(line, "+flags:") == 0) { in_flags_block = 1; continue; }
        
        if (in_flags_block) {
            if (strcmp(line, ";") == 0) { in_flags_block = 0; current_flag = NULL; continue; }
            
            char *trimmed = original;
            while (isspace((unsigned char)*trimmed)) trimmed++;
            if ((strncmp(trimmed, "-fg(", 4) == 0 || strncmp(trimmed, "+fg(", 4) == 0)) {
                add_flag(current_cmd, trimmed);
                current_flag = current_cmd->flags;
                in_flag = 1;
                continue;
            }
            if (strcmp(line, "eofg") == 0) { current_flag = NULL; in_flag = 0; continue; }
            if (in_flag && current_flag) { add_flag_command(current_flag, line); continue; }
        }
        
        if (!in_flags_block) add_main_command(current_cmd, line);
    }
    fclose(fp);
}

void add_job(const char *cmd) {
    Job *job = malloc(sizeof(Job));
    job->command = strdup(cmd);
    job->next = job_queue;
    job_queue = job;
}

void run_jobs_parallel(void) {
    Job *job = job_queue;
    int active_jobs = 0;
    
    while (job || active_jobs > 0) {
        while (active_jobs < jobs && job) {
            pid_t pid = fork();
            if (pid == 0) { execl("/bin/sh", "sh", "-c", job->command, NULL); exit(1); }
            else if (pid > 0) { job->pid = pid; active_jobs++; job = job->next; }
        }
        int status;
        pid_t done = wait(&status);
        Job *j = job_queue;
        while (j) { if (j->pid == done) { j->ret = WEXITSTATUS(status); active_jobs--; break; } j = j->next; }
    }
    
    Job *j = job_queue;
    while (j) {
        if (j->ret != 0) {
            char msg[MAX_LINE];
            snprintf(msg, sizeof(msg), "Error: %s", j->command);
            print_color(COLOR_RED, msg);
            exit(j->ret);
        }
        j = j->next;
    }
}

void clear_jobs(void) {
    Job *j = job_queue;
    while (j) { Job *next = j->next; free(j->command); free(j); j = next; }
    job_queue = NULL;
}

int execute_single_command(const char *cmd) {
    if (dry_run) { printf("%s\n", cmd); return 0; }
    int ret = system(cmd);
    if (ret != 0) {
        char msg[MAX_LINE];
        snprintf(msg, sizeof(msg), "Error: %s", cmd);
        print_color(COLOR_RED, msg);
        return ret;
    }
    return 0;
}

int execute_commands(char **cmds, int cmd_count) {
    if (jobs > 1) {
        for (int i = 0; i < cmd_count; i++) add_job(cmds[i]);
        run_jobs_parallel();
        clear_jobs();
        return 0;
    } else {
        for (int i = 0; i < cmd_count; i++) {
            int ret = execute_single_command(cmds[i]);
            if (ret != 0) return ret;
        }
        return 0;
    }
}

int execute_flag(Flag *flag) {
    for (int i = 0; i < flag->cmd_count; i++) {
        char *expanded = expand_string(flag->commands[i]);
        int ret = execute_single_command(expanded);
        if (ret != 0) return ret;
    }
    return 0;
}

int execute_pattern_rule(const char *target, PatternRule *rule) {
    if (!target || strlen(target) == 0) return 0;
    
    char clean_target[MAX_LINE];
    strcpy(clean_target, target);
    trim(clean_target);
    
    if (strlen(clean_target) == 0) return 0;
    
    char *stem = apply_pattern(clean_target, rule->target_pattern);
    if (!stem) return 0;
    
    char *dep_pattern_expanded = expand_dep_pattern(rule->dep_pattern, stem);
    
    int dep_count;
    char **deps = split_deps(dep_pattern_expanded, &dep_count);
    
    // Компилируем зависимости рекурсивно
    for (int i = 0; i < dep_count; i++) {
        PatternRule *dep_rule = find_pattern_rule(deps[i]);
        if (dep_rule) {
            int ret = execute_pattern_rule(deps[i], dep_rule);
            if (ret != 0) return ret;
        } else {
            Command *cmd = find_command(deps[i]);
            if (cmd && cmd->is_rule) {
                int ret = execute_target(deps[i]);
                if (ret != 0) return ret;
            }
        }
    }
    
    if (!needs_rebuild(target, deps, dep_count) && !dry_run) {
        for (int i = 0; i < dep_count; i++) free(deps[i]);
        free(deps);
        return 0;
    }
    
    set_special_vars(target, dep_count > 0 ? deps[0] : "", dep_pattern_expanded);
    
    int ret = 0;
    for (int i = 0; i < rule->cmd_count; i++) {
        char *expanded = expand_string_with_special(rule->commands[i], target, 
                                                     dep_count > 0 ? deps[0] : "", dep_pattern_expanded);
        if (execute_single_command(expanded) != 0) {
            ret = 1;
            break;
        }
    }
    
    for (int i = 0; i < dep_count; i++) free(deps[i]);
    free(deps);
    return ret;
}

int execute_target(const char *target_name) {
    // Очищаем от пробелов и проверяем
    if (!target_name) return 0;
    
    char clean_target[MAX_LINE];
    strcpy(clean_target, target_name);
    trim(clean_target);
    
    if (strlen(clean_target) == 0) return 0;
    
    Command *cmd = find_command(clean_target);
    if (cmd) {
        // Выполняем команду/правило
        if (cmd->is_rule && cmd->dep_count > 0) {
            // Проверяем зависимости
            int need_build = 1;
            if (!cmd->is_phony) {
                need_build = needs_rebuild(clean_target, cmd->deps, cmd->dep_count);
            }
            
            if (!need_build && !dry_run) return 0;
            
            // Сначала выполняем зависимости
            for (int i = 0; i < cmd->dep_count; i++) {
                int ret = execute_target(cmd->deps[i]);
                if (ret != 0) return ret;
            }
        }
        
        // Выполняем команды
        for (int i = 0; i < cmd->main_cmd_count; i++) {
            char *line = cmd->main_commands[i];
            char *expanded = expand_string(line);
            
            // Очищаем от пробелов
            char clean_line[MAX_LINE];
            strcpy(clean_line, expanded);
            trim(clean_line);
            
            if (strlen(clean_line) == 0) continue;
            
            // Если строка начинается с "call " — рекурсивный вызов
            if (strncmp(clean_line, "call ", 5) == 0) {
                char *target = clean_line + 5;
                trim(target);
                if (strlen(target) > 0) {
                    int ret = execute_target(target);
                    if (ret != 0) return ret;
                }
            } else {
                int ret = execute_single_command(clean_line);
                if (ret != 0) return ret;
            }
        }
        return 0;
    }
    
    // Ищем паттерн-правило
    PatternRule *rule = find_pattern_rule(clean_target);
    if (rule) {
        return execute_pattern_rule(clean_target, rule);
    }
    
    // Если файл существует — ничего не делаем
    if (get_mtime(clean_target) != 0) return 0;
    
    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), "Unknown target: %s", clean_target);
    print_color(COLOR_RED, msg);
    return 1;
}

int execute_command(const char *cmd_name, int flag_count, char **flags) {
    // Очищаем имя команды
    if (!cmd_name) return 0;
    
    char clean_cmd[MAX_LINE];
    strcpy(clean_cmd, cmd_name);
    trim(clean_cmd);
    
    if (strlen(clean_cmd) == 0) return 0;
    
    Command *cmd = find_command(clean_cmd);
    if (!cmd) {
        return execute_target(clean_cmd);
    }
    
    // Если это правило с зависимостями — обрабатываем как цель
    if (cmd->is_rule && cmd->dep_count > 0) {
        return execute_target(clean_cmd);
    }
    
    Flag *requested_flags[MAX_NAME];
    int requested_count = 0;
    
    for (int i = 0; i < flag_count; i++) {
        char *flag_name = flags[i];
        if (flag_name[0] == '-') {
            if (flag_name[1] == '-') flag_name += 2;
            else flag_name += 1;
        }
        
        Flag *f = cmd->flags;
        int found = 0;
        while (f) {
            if (strcmp(f->name, flag_name) == 0) {
                requested_flags[requested_count++] = f;
                found = 1;
                break;
            }
            f = f->next;
        }
        if (!found) {
            char msg[MAX_LINE];
            snprintf(msg, sizeof(msg), "Unknown flag: %s", flags[i]);
            print_color(COLOR_RED, msg);
            return 1;
        }
    }
    
    // BEFORE flags
    for (int i = 0; i < requested_count; i++) {
        if (requested_flags[i]->type == 0) {
            int ret = execute_flag(requested_flags[i]);
            if (ret != 0) return ret;
        }
    }
    
    // Выполняем команды
    for (int i = 0; i < cmd->main_cmd_count; i++) {
        char *line = cmd->main_commands[i];
        char *expanded = expand_string(line);
        
        char clean_line[MAX_LINE];
        strcpy(clean_line, expanded);
        trim(clean_line);
        
        if (strlen(clean_line) == 0) continue;
        
        // Если строка начинается с "call " — вызов цели
        if (strncmp(clean_line, "call ", 5) == 0) {
            char *target = clean_line + 5;
            trim(target);
            if (strlen(target) > 0) {
                int ret = execute_target(target);
                if (ret != 0) return ret;
            }
        } else {
            int ret = execute_single_command(clean_line);
            if (ret != 0) return ret;
        }
    }
    
    // AFTER flags
    for (int i = 0; i < requested_count; i++) {
        if (requested_flags[i]->type == 1) {
            int ret = execute_flag(requested_flags[i]);
            if (ret != 0) return ret;
        }
    }
    
    return 0;
}

void free_commands() {
    Command *cmd = commands;
    while (cmd) {
        Command *next = cmd->next;
        for (int i = 0; i < cmd->main_cmd_count; i++) free(cmd->main_commands[i]);
        free(cmd->main_commands);
        for (int i = 0; i < cmd->dep_count; i++) free(cmd->deps[i]);
        free(cmd->deps);
        Flag *flag = cmd->flags;
        while (flag) {
            Flag *next_flag = flag->next;
            for (int i = 0; i < flag->cmd_count; i++) free(flag->commands[i]);
            free(flag->commands);
            free(flag);
            flag = next_flag;
        }
        free(cmd);
        cmd = next;
    }
    
    Variable *var = variables;
    while (var) {
        Variable *next = var->next;
        free(var->value);
        free(var);
        var = next;
    }
    
    PatternRule *rule = pattern_rules;
    while (rule) {
        PatternRule *next = rule->next;
        free(rule->target_pattern);
        free(rule->dep_pattern);
        for (int i = 0; i < rule->cmd_count; i++) free(rule->commands[i]);
        free(rule->commands);
        free(rule);
        rule = next;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_color(COLOR_YELLOW, "Usage: umk <command> [flags...]");
        return 1;
    }
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-j") == 0 && i+1 < argc) { jobs = atoi(argv[++i]); if (jobs < 1) jobs = 1; }
        else if (strcmp(argv[i], "--no-color") == 0) use_color = 0;
        else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0) dry_run = 1;
    }
    
    parse_umkfile("UMK");
    
    int flag_count = 0;
    char **flags = malloc((argc - 2) * sizeof(char*));
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--no-color") == 0 ||
            strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0) {
            if (strcmp(argv[i], "-j") == 0) i++;
            continue;
        }
        flags[flag_count++] = argv[i];
    }
    
    int ret = execute_command(argv[1], flag_count, flags);
    free(flags);
    free_commands();
    return ret;
}
