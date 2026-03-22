// umk.c
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
#include <errno.h>
#include <pthread.h>

#define MAX_LINE 4096
#define MAX_NAME 256
#define MAX_JOBS 64

#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_RESET   "\033[0m"

typedef enum {
    NODE_VAR,
    NODE_STRING,
    NODE_FUNC_CALL
} NodeType;

typedef struct ExprNode {
    NodeType type;
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
    time_t last_build;
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

void trim(char *str);
int is_blank(const char *str);
char *expand_string(const char *str, int for_dependency);
char *expand_string_with_special(const char *str, const char *target, const char *dep, const char *all_deps);  // добавить эту строку
ExprNode *parse_expr(const char *str);
char *eval_expr(ExprNode *node);
void free_expr(ExprNode *node);
void add_variable(const char *name, const char *value);
char *get_variable(const char *name);
void set_special_vars(const char *target, const char *dep, const char *all_deps);
void add_command(const char *name);
void add_main_command(Command *cmd, const char *line);
void add_flag(Command *cmd, const char *flag_line);
void add_flag_command(Flag *flag, const char *line);
void add_pattern_rule(const char *target, const char *dep, const char *cmd);
void parse_umkfile(const char *filename);
int evaluate_condition(const char *expr);
int execute_command(const char *cmd_name, int argc, char **argv);
int execute_commands(char **cmds, int cmd_count, int check_timestamp);
int execute_single_command(const char *cmd);
int execute_flag(Flag *flag);
int execute_pattern_rule(const char *target, PatternRule *rule);
int needs_rebuild(const char *target, char **deps, int dep_count);
time_t get_mtime(const char *path);
void collect_dependencies(const char *pattern, char ***deps, int *dep_count);
char **split_deps(const char *dep_str, int *count);
void run_jobs_parallel(void);
void add_job(const char *cmd);
void clear_jobs(void);
char *wildcard(const char *pattern);
char *shell(const char *cmd);
void print_color(const char *color, const char *msg);
int match_pattern(const char *name, const char *pattern);
char *apply_pattern(const char *target, const char *pattern);

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

void trim(char *str) {
    char *start = str;
    char *end;
    
    while (isspace((unsigned char)*start)) start++;
    
    if (*start == 0) {
        str[0] = '\0';
        return;
    }
    
    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    
    memmove(str, start, end - start + 1);
    str[end - start + 1] = '\0';
}

int is_blank(const char *str) {
    while (*str) {
        if (!isspace((unsigned char)*str)) return 0;
        str++;
    }
    return 1;
}

time_t get_mtime(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return st.st_mtime;
    }
    return 0;
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
        if (strcmp(var->name, name) == 0) {
            return var->value;
        }
        var = var->next;
    }
    return NULL;
}

static char current_target[MAX_NAME];
static char current_dep[MAX_NAME];
static char current_deps[MAX_LINE];

void set_special_vars(const char *target, const char *dep, const char *all_deps) {
    if (target) {
        strcpy(current_target, target);
        add_variable("@", target);
    }
    if (dep) {
        strcpy(current_dep, dep);
        add_variable("<", dep);
    }
    if (all_deps) {
        strcpy(current_deps, all_deps);
        add_variable("^", all_deps);
    }
}

int match_pattern(const char *name, const char *pattern) {
    const char *p = pattern;
    const char *n = name;
    
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return 1;
            while (*n) {
                if (match_pattern(n, p)) return 1;
                n++;
            }
            return 0;
        } else if (*p != *n) {
            return 0;
        }
        p++;
        n++;
    }
    
    return *n == '\0';
}

char *apply_pattern(const char *target, const char *pattern) {
    static char result[MAX_LINE];
    char *star_pos = (char*)strchr(pattern, '*');
    if (!star_pos) {
        strcpy(result, pattern);
        return result;
    }
    
    int prefix_len = star_pos - pattern;
    int suffix_len = strlen(pattern) - (prefix_len + 1);
    
    if (strncmp(target, pattern, prefix_len) != 0) return NULL;
    
    int target_len = strlen(target);
    if (target_len < prefix_len + suffix_len) return NULL;
    
    if (suffix_len > 0) {
        const char *suffix = pattern + prefix_len + 1;
        if (strcmp(target + target_len - suffix_len, suffix) != 0) return NULL;
    }
    
    char stem[MAX_LINE];
    int stem_len = target_len - prefix_len - suffix_len;
    strncpy(stem, target + prefix_len, stem_len);
    stem[stem_len] = '\0';
    
    strcpy(result, stem);
    return result;
}

void collect_dependencies(const char *pattern, char ***deps, int *dep_count) {
    *deps = NULL;
    *dep_count = 0;
    
    char *star_pos = (char*)strchr(pattern, '*');
    if (!star_pos) {
        *deps = malloc(sizeof(char*));
        (*deps)[0] = strdup(pattern);
        *dep_count = 1;
        return;
    }
    
    int prefix_len = star_pos - pattern;
    int suffix_len = strlen(pattern) - (prefix_len + 1);
    char suffix[MAX_LINE];
    if (suffix_len > 0) {
        strcpy(suffix, pattern + prefix_len + 1);
    } else {
        suffix[0] = '\0';
    }
    
    DIR *dir = opendir(".");
    if (!dir) return;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        
        if (strncmp(entry->d_name, pattern, prefix_len) == 0) {
            int name_len = strlen(entry->d_name);
            if (suffix_len == 0 || (name_len >= suffix_len && 
                strcmp(entry->d_name + name_len - suffix_len, suffix) == 0)) {
                (*deps) = realloc(*deps, (*dep_count + 1) * sizeof(char*));
                (*deps)[*dep_count] = strdup(entry->d_name);
                (*dep_count)++;
            }
        }
    }
    
    closedir(dir);
}

int needs_rebuild(const char *target, char **deps, int dep_count) {
    time_t target_time = get_mtime(target);
    
    if (target_time == 0) return 1;
    
    for (int i = 0; i < dep_count; i++) {
        time_t dep_time = get_mtime(deps[i]);
        if (dep_time > target_time) return 1;
    }
    
    return 0;
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
    if (arg_count < 1) return "";
    return wildcard(args[0]);
}

char *func_shell(const char **args, int arg_count) {
    if (arg_count < 1) return "";
    return shell(args[0]);
}

ExprNode *parse_expr(const char *str) {
    ExprNode *node = malloc(sizeof(ExprNode));
    node->args = NULL;
    node->arg_count = 0;
    
    if (str[0] == '$' && str[1] == '(') {
        const char *end = strchr(str, ')');
        if (!end) {
            node->type = NODE_STRING;
            node->value = strdup(str);
            return node;
        }
        
        char *inner = malloc(end - str - 1);
        strncpy(inner, str + 2, end - str - 2);
        inner[end - str - 2] = '\0';
        
        char *space = strchr(inner, ' ');
        if (space) {
            *space = '\0';
            node->type = NODE_FUNC_CALL;
            node->func_name = strdup(inner);
            
            char *args_str = space + 1;
            node->arg_count = 1;
            for (char *p = args_str; *p; p++) {
                if (*p == ' ') node->arg_count++;
            }
            
            node->args = malloc(node->arg_count * sizeof(ExprNode*));
            char *arg = strtok(args_str, " ");
            int i = 0;
            while (arg) {
                node->args[i] = parse_expr(arg);
                i++;
                arg = strtok(NULL, " ");
            }
        } else {
            node->type = NODE_VAR;
            node->value = strdup(inner);
        }
        
        free(inner);
    } else {
        node->type = NODE_STRING;
        node->value = strdup(str);
    }
    
    return node;
}

char *eval_expr(ExprNode *node) {
    switch (node->type) {
        case NODE_VAR: {
            char *val = get_variable(node->value);
            if (val) return val;
            return "";
        }
        case NODE_STRING:
            return node->value;
        case NODE_FUNC_CALL: {
            for (int i = 0; builtins[i].name; i++) {
                if (strcmp(node->func_name, builtins[i].name) == 0) {
                    const char **args = malloc(node->arg_count * sizeof(char*));
                    for (int j = 0; j < node->arg_count; j++) {
                        args[j] = eval_expr(node->args[j]);
                    }
                    char *result = builtins[i].func(args, node->arg_count);
                    free(args);
                    return result;
                }
            }
            return "";
        }
    }
    return "";
}

void free_expr(ExprNode *node) {
    if (!node) return;
    
    if (node->type == NODE_FUNC_CALL) {
        free(node->func_name);
        for (int i = 0; i < node->arg_count; i++) {
            free_expr(node->args[i]);
        }
        free(node->args);
    } else {
        free(node->value);
    }
    
    free(node);
}

char *expand_string(const char *str, int for_dependency) {
    static char result[MAX_LINE];
    result[0] = '\0';
    
    (void)for_dependency;  // подавляем warning о неиспользуемом параметре
    
    const char *p = str;
    while (*p) {
        if (*p == '$' && *(p+1) == '(') {
            ExprNode *expr = parse_expr(p);
            char *val = eval_expr(expr);
            strcat(result, val);
            
            int depth = 1;
            p += 2;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                p++;
            }
            
            free_expr(expr);
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
    strcpy(expanded, expand_string(expr, 0));
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

void add_command(const char *name) {
    Command *new_cmd = malloc(sizeof(Command));
    strcpy(new_cmd->name, name);
    new_cmd->main_commands = NULL;
    new_cmd->main_cmd_count = 0;
    new_cmd->flags = NULL;
    new_cmd->next = commands;
    new_cmd->is_phony = 0;
    new_cmd->last_build = 0;
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
    char *expanded = expand_string(line, 0);
    
    cmd->main_commands = realloc(cmd->main_commands, 
                                   (cmd->main_cmd_count + 1) * sizeof(char*));
    cmd->main_commands[cmd->main_cmd_count] = strdup(expanded);
    cmd->main_cmd_count++;
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
    
    if (strncmp(line_copy, "-fg(", 4) == 0) {
        new_flag->type = 0;
    } else {
        new_flag->type = 1;
    }
    
    char *start = strchr(line_copy, '(') + 1;
    char *end = strchr(start, ')');
    int len = end - start;
    strncpy(new_flag->name, start, len);
    new_flag->name[len] = '\0';
}

void add_flag_command(Flag *flag, const char *line) {
    char *expanded = expand_string(line, 0);
    
    flag->commands = realloc(flag->commands, 
                              (flag->cmd_count + 1) * sizeof(char*));
    flag->commands[flag->cmd_count] = strdup(expanded);
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

void parse_umkfile(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        print_color(COLOR_RED, "No UMK file found");
        exit(1);
    }
    
    char line[MAX_LINE];
    Command *current_cmd = NULL;
    Flag *current_flag = NULL;
    int in_flags_block = 0;
    int in_flag = 0;
    int in_condition = 0;
    int condition_result = 1;
    int skip_until_endif = 0;
    
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
            if (in_condition) {
                skip_until_endif = condition_result;
                condition_result = !condition_result;
            }
            continue;
        }
        
        if (strcmp(line, "endif") == 0) {
            in_condition = 0;
            skip_until_endif = 0;
            condition_result = 1;
            continue;
        }
        
        if (skip_until_endif) continue;
        
        char *eq = strchr(line, '=');
        if (eq && !in_flags_block && !current_cmd && !in_condition) {
            *eq = '\0';
            char *var_name = line;
            char *var_value = eq + 1;
            trim(var_name);
            trim(var_value);
            add_variable(var_name, expand_string(var_value, 0));
            continue;
        }
        
        char *colon = strchr(line, ':');
        if (colon && strchr(line, '%') && !in_flags_block && !current_cmd) {
            *colon = '\0';
            char *target = line;
            char *dep = colon + 1;
            trim(target);
            trim(dep);
            
            if (fgets(line, sizeof(line), fp)) {
                trim(line);
                if (line[0] == '\t' || line[0] == ' ') {
                    add_pattern_rule(target, dep, line + 1);
                }
            }
            continue;
        }
        
        if (strcmp(line, "eoc") == 0) {
            if (current_cmd) {
                current_cmd->is_phony = 1;
            }
            current_cmd = NULL;
            in_flags_block = 0;
            in_flag = 0;
            continue;
        }
        
        colon = strchr(line, ':');
        if (colon && !in_flags_block && strcmp(line, "+flags:") != 0) {
            int in_quotes = 0;
            int is_command = 1;
            for (char *p = line; p < colon; p++) {
                if (*p == '"') in_quotes = !in_quotes;
                if (in_quotes) {
                    is_command = 0;
                    break;
                }
            }
            
            if (is_command) {
                *colon = '\0';
                add_command(line);
                current_cmd = commands;
                continue;
            }
        }
        
        if (!current_cmd) continue;
        
        if (strcmp(line, "+flags:") == 0) {
            in_flags_block = 1;
            continue;
        }
        
        if (in_flags_block) {
            if (strcmp(line, ";") == 0) {
                in_flags_block = 0;
                current_flag = NULL;
                continue;
            }
            
            char *trimmed = original;
            while (isspace((unsigned char)*trimmed)) trimmed++;
            
            if ((strncmp(trimmed, "-fg(", 4) == 0 || strncmp(trimmed, "+fg(", 4) == 0)) {
                add_flag(current_cmd, trimmed);
                current_flag = current_cmd->flags;
                in_flag = 1;
                continue;
            }
            
            if (strcmp(line, "eofg") == 0) {
                current_flag = NULL;
                in_flag = 0;
                continue;
            }
            
            if (in_flag && current_flag) {
                add_flag_command(current_flag, line);
                continue;
            }
        }
        
        if (!in_flags_block) {
            add_main_command(current_cmd, line);
        }
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
            if (pid == 0) {
                execl("/bin/sh", "sh", "-c", job->command, NULL);
                exit(1);
            } else if (pid > 0) {
                job->pid = pid;
                active_jobs++;
                job = job->next;
            }
        }
        
        int status;
        pid_t done = wait(&status);
        
        Job *j = job_queue;
        while (j) {
            if (j->pid == done) {
                j->ret = WEXITSTATUS(status);
                active_jobs--;
                break;
            }
            j = j->next;
        }
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
    while (j) {
        Job *next = j->next;
        free(j->command);
        free(j);
        j = next;
    }
    job_queue = NULL;
}

int execute_single_command(const char *cmd) {
    if (dry_run) {
        printf("%s\n", cmd);
        return 0;
    }
    
    int ret = system(cmd);
    if (ret != 0) {
        char msg[MAX_LINE];
        snprintf(msg, sizeof(msg), "Error: %s", cmd);
        print_color(COLOR_RED, msg);
        return ret;
    }
    return 0;
}

int execute_commands(char **cmds, int cmd_count, int check_timestamp) {
    (void)check_timestamp;  // подавляем warning о неиспользуемом параметре
    
    if (jobs > 1) {
        for (int i = 0; i < cmd_count; i++) {
            add_job(cmds[i]);
        }
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
    return execute_commands(flag->commands, flag->cmd_count, 0);
}

int execute_pattern_rule(const char *target, PatternRule *rule) {
    char *stem = apply_pattern(target, rule->target_pattern);
    if (!stem) return 0;
    
    char dep_pattern[MAX_LINE];
    strcpy(dep_pattern, rule->dep_pattern);
    
    char *dep_result = malloc(MAX_LINE);
    char *p = dep_pattern;
    char *out = dep_result;
    while (*p) {
        if (*p == '*' && (p == dep_pattern || *(p-1) != '$')) {
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
    
    int dep_count;
    char **deps = split_deps(dep_result, &dep_count);
    
    if (!needs_rebuild(target, deps, dep_count) && !dry_run) {
        for (int i = 0; i < dep_count; i++) free(deps[i]);
        free(deps);
        free(dep_result);
        return 0;
    }
    
    set_special_vars(target, dep_count > 0 ? deps[0] : "", dep_result);
    
    int ret = 0;
    for (int i = 0; i < rule->cmd_count; i++) {
        char *expanded = expand_string(rule->commands[i], 0);
        if (execute_single_command(expanded) != 0) {
            ret = 1;
            break;
        }
    }
    
    for (int i = 0; i < dep_count; i++) free(deps[i]);
    free(deps);
    free(dep_result);
    
    return ret;
}

int execute_command(const char *cmd_name, int flag_count, char **flags) {
    Command *cmd = find_command(cmd_name);
    if (!cmd) {
        char msg[MAX_LINE];
        snprintf(msg, sizeof(msg), "Unknown command: %s", cmd_name);
        print_color(COLOR_RED, msg);
        return 1;
    }
    
    Flag *requested_flags[MAX_NAME];
    int requested_count = 0;
    
    for (int i = 0; i < flag_count; i++) {
        char *flag_name = flags[i];
        
        if (flag_name[0] == '-') {
            if (flag_name[1] == '-') {
                flag_name += 2;
            } else {
                flag_name += 1;
            }
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
    
    // Собираем зависимости для специальных переменных
    char *target = strdup(cmd_name);
    char *first_dep = NULL;
    char *all_deps = NULL;
    
    // Пытаемся найти зависимости из паттерн-правил
    PatternRule *rule = pattern_rules;
    while (rule) {
        if (match_pattern(cmd_name, rule->target_pattern)) {
            char *stem = apply_pattern(cmd_name, rule->target_pattern);
            if (stem) {
                char dep_pattern[MAX_LINE];
                strcpy(dep_pattern, rule->dep_pattern);
                
                char *dep_result = malloc(MAX_LINE);
                char *p = dep_pattern;
                char *out = dep_result;
                while (*p) {
                    if (*p == '*' && (p == dep_pattern || *(p-1) != '$')) {
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
                
                int dep_count;
                char **deps = split_deps(dep_result, &dep_count);
                if (dep_count > 0) {
                    first_dep = strdup(deps[0]);
                    
                    // Собираем все зависимости в строку
                    all_deps = malloc(MAX_LINE);
                    all_deps[0] = '\0';
                    for (int i = 0; i < dep_count; i++) {
                        if (i > 0) strcat(all_deps, " ");
                        strcat(all_deps, deps[i]);
                    }
                }
                
                for (int i = 0; i < dep_count; i++) free(deps[i]);
                free(deps);
                free(dep_result);
                break;
            }
        }
        rule = rule->next;
    }
    
    // Если нет зависимостей из правил, используем пустые
    if (!first_dep) first_dep = strdup("");
    if (!all_deps) all_deps = strdup("");
    
    set_special_vars(target, first_dep, all_deps);
    
    Flag *current = cmd->flags;
    while (current) {
        for (int i = 0; i < requested_count; i++) {
            if (requested_flags[i] == current && current->type == 0) {
                int ret = execute_flag(current);
                if (ret != 0) {
                    free(target);
                    free(first_dep);
                    free(all_deps);
                    return ret;
                }
                break;
            }
        }
        current = current->next;
    }
    
    int ret = 0;
    int need_build = 1;
    
    if (!cmd->is_phony && cmd->main_cmd_count > 0) {
        need_build = 0;
        for (int i = 0; i < cmd->main_cmd_count; i++) {
            if (cmd->main_commands[i][0] != '\0') {
                need_build = 1;
                break;
            }
        }
    }
    
    if (need_build) {
        // Расширяем команды с учётом специальных переменных
        char **expanded_commands = malloc(cmd->main_cmd_count * sizeof(char*));
        for (int i = 0; i < cmd->main_cmd_count; i++) {
            char *exp = expand_string_with_special(cmd->main_commands[i], target, first_dep, all_deps);
            expanded_commands[i] = exp;
        }
        ret = execute_commands(expanded_commands, cmd->main_cmd_count, 1);
        for (int i = 0; i < cmd->main_cmd_count; i++) {
            free(expanded_commands[i]);
        }
        free(expanded_commands);
        if (ret != 0) {
            free(target);
            free(first_dep);
            free(all_deps);
            return ret;
        }
    }
    
    current = cmd->flags;
    while (current) {
        for (int i = 0; i < requested_count; i++) {
            if (requested_flags[i] == current && current->type == 1) {
                int ret = execute_flag(current);
                if (ret != 0) {
                    free(target);
                    free(first_dep);
                    free(all_deps);
                    return ret;
                }
                break;
            }
        }
        current = current->next;
    }
    
    free(target);
    free(first_dep);
    free(all_deps);
    
    return 0;
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
            ExprNode *expr = parse_expr(p);
            char *val = eval_expr(expr);
            strcat(result, val);
            
            int depth = 1;
            p += 2;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                p++;
            }
            
            free_expr(expr);
        } else {
            char ch[2] = {*p, '\0'};
            strcat(result, ch);
            p++;
        }
    }
    
    return result;
}

void free_commands() {
    Command *cmd = commands;
    while (cmd) {
        Command *next_cmd = cmd->next;
        
        for (int i = 0; i < cmd->main_cmd_count; i++) {
            free(cmd->main_commands[i]);
        }
        free(cmd->main_commands);
        
        Flag *flag = cmd->flags;
        while (flag) {
            Flag *next_flag = flag->next;
            
            for (int i = 0; i < flag->cmd_count; i++) {
                free(flag->commands[i]);
            }
            free(flag->commands);
            free(flag);
            
            flag = next_flag;
        }
        
        free(cmd);
        cmd = next_cmd;
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
        for (int i = 0; i < rule->cmd_count; i++) {
            free(rule->commands[i]);
        }
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
        if (strcmp(argv[i], "-j") == 0 && i+1 < argc) {
            jobs = atoi(argv[++i]);
            if (jobs < 1) jobs = 1;
        } else if (strcmp(argv[i], "--no-color") == 0) {
            use_color = 0;
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        }
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
