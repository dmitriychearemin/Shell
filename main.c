#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <ctype.h>

#define MAX_ARGS 64
#define MAX_CMD_LEN 1024
#define MAX_PIPES 10
#define MAX_HISTORY 100

typedef struct {
    char *args[MAX_ARGS];
    char *input_file;
    char *output_file;
    int append;
    int background;
} Command;

volatile sig_atomic_t foreground_mode = 1;
char *history[MAX_HISTORY];
int history_count = 0;

struct termios orig_termios;

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void add_to_history(const char *cmd) {
    if (cmd[0] == '\0') return;
    
    if (history_count < MAX_HISTORY) {
        history[history_count] = strdup(cmd);
    } else {
        free(history[0]);
        for (int i = 0; i < MAX_HISTORY-1; i++) {
            history[i] = history[i+1];
        }
        history[MAX_HISTORY-1] = strdup(cmd);
    }
    history_count++;
}

void show_history() {
    int start = history_count > MAX_HISTORY ? history_count - MAX_HISTORY : 0;
    for (int i = start; i < history_count; i++) {
        printf("%d: %s\n", i+1, history[i % MAX_HISTORY]);
    }
}

int execute_builtin(Command *cmd) {
    if (strcmp(cmd->args[0], "cd") == 0) {
        char *path = cmd->args[1] ? cmd->args[1] : getenv("HOME");
        if (chdir(path)) {
            perror("cd failed");
        }
        return 1;
    }
    
    if (strcmp(cmd->args[0], "help") == 0) {
        printf("Built-in commands:\n"
               "cd [DIR] - change directory\n"
               "help - show help\n"
               "history - show command history\n"
               "exit - exit shell\n");
        return 1;
    }
    
    if (strcmp(cmd->args[0], "history") == 0) {
        show_history();
        return 1;
    }
    
    return 0;
}

void sigint_handler(int sig) {
    if (foreground_mode) {
        printf("\nmyshell> ");
        fflush(stdout);
    }
}

char *read_line() {
    enableRawMode();
    
    static char line[MAX_CMD_LEN];
    char saved_line[MAX_CMD_LEN] = {0};
    int len = 0;
    int pos = 0;
    int history_index = 0;
    
    memset(line, 0, sizeof(line));
    
    while (1) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            len = 0;
            break;
        }
        
        if (c == '\n') {
            printf("\n");
            break;
        } else if (c == '\x1B') {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) break;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) break;
            
            if (seq[0] == '[') {
                switch (seq[1]) {
                    case 'A':
                        if (history_index < history_count) {
                            if (history_index == 0) {
                                strncpy(saved_line, line, MAX_CMD_LEN);
                            }
                            history_index++;
                            int idx = history_count - history_index;
                            if (idx < 0) idx = 0;
                            if (idx < history_count) {
                                strncpy(line, history[idx], MAX_CMD_LEN);
                                len = strlen(line);
                                pos = len;
                                printf("\033[2K\r");
                                printf("myshell> %s", line);
                                fflush(stdout);
                            }
                        }
                        break;
                    case 'B':
                        if (history_index > 0) {
                            history_index--;
                            if (history_index == 0) {
                                strncpy(line, saved_line, MAX_CMD_LEN);
                                len = strlen(line);
                                pos = len;
                            } else {
                                int idx = history_count - history_index;
                                if (idx < 0) idx = 0;
                                if (idx < history_count) {
                                    strncpy(line, history[idx], MAX_CMD_LEN);
                                    len = strlen(line);
                                    pos = len;
                                }
                            }
                            printf("\033[2K\r");
                            printf("myshell> %s", line);
                            fflush(stdout);
                        }
                        break;
                    case 'C':
                        if (pos < len) {
                            pos++;
                            printf("\033[1C");
                            fflush(stdout);
                        }
                        break;
                    case 'D':
                        if (pos > 0) {
                            pos--;
                            printf("\033[1D");
                            fflush(stdout);
                        }
                        break;
                }
            }
        } else if (c == 127 || c == 8) {
            if (pos > 0 && len > 0) {
                memmove(&line[pos-1], &line[pos], len - pos + 1);
                len--;
                pos--;
                line[len] = '\0';
                printf("\033[2K\r");
                printf("myshell> %s", line);
                printf("\033[%dG", pos + 8 + 1);
                fflush(stdout);
            }
        } else if (isprint(c)) {
            if (len < MAX_CMD_LEN - 1) {
                memmove(&line[pos+1], &line[pos], len - pos + 1);
                line[pos] = c;
                len++;
                pos++;
                line[len] = '\0';
                printf("\033[2K\r");
                printf("myshell> %s", line);
                printf("\033[%dG", pos + 8 + 1);
                fflush(stdout);
            }
        }
    }
    
    disableRawMode();
    
    if (len > 0) {
        add_to_history(line);
    }
    
    return strdup(line);
}

int parse_pipeline(char *line, Command pipeline[]) {
    int cmd_count = 0;
    char *saveptr = NULL;

    char *line_copy = strdup(line);
    char *token = strtok_r(line_copy, "|", &saveptr);

    while (token != NULL && cmd_count < MAX_PIPES) {
        // Trim whitespace
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && *end == ' ') end--;
        *(end + 1) = '\0';

        if (strlen(token) == 0) {
            token = strtok_r(NULL, "|", &saveptr);
            continue;
        }

        // Command initialization
        pipeline[cmd_count].input_file = NULL;
        pipeline[cmd_count].output_file = NULL;
        pipeline[cmd_count].append = 0;
        pipeline[cmd_count].background = 0;

        // Parse command arguments
        char *arg_saveptr = NULL;
        char *arg = strtok_r(token, " \t", &arg_saveptr);
        int i = 0;

        while (arg != NULL && i < MAX_ARGS - 1) {
            // Handle redirection operators
            if (strcmp(arg, ">") == 0 || strcmp(arg, ">>") == 0) {
                // Get filename
                char *file = strtok_r(NULL, " \t", &arg_saveptr);
                if (!file) break;
                
                // Handle quotes in filename
                if (file[0] == '"') {
                    file = strtok_r(NULL, "\"", &arg_saveptr);
                    strtok_r(NULL, " \t", &arg_saveptr); // Skip remaining
                }

                if (file) {
                    pipeline[cmd_count].output_file = strdup(file);
                    pipeline[cmd_count].append = (strcmp(arg, ">>") == 0);
                }
                break;
            }
            else if (strcmp(arg, "<") == 0) {
                char *file = strtok_r(NULL, " \t", &arg_saveptr);
                if (file) {
                    pipeline[cmd_count].input_file = strdup(file);
                }
                break;
            }
            else if (strcmp(arg, "&") == 0) {
                pipeline[cmd_count].background = 1;
            }
            else {
                // Handle quoted arguments
                if (arg[0] == '"') {
                    char *end_quote = strchr(arg + 1, '"');
                    if (end_quote) {
                        *end_quote = '\0';
                        pipeline[cmd_count].args[i++] = strdup(arg + 1);
                        arg = end_quote + 1;
                        continue;
                    }
                }
                pipeline[cmd_count].args[i++] = strdup(arg);
            }
            arg = strtok_r(NULL, " \t", &arg_saveptr);
        }
        pipeline[cmd_count].args[i] = NULL;
        cmd_count++;
        token = strtok_r(NULL, "|", &saveptr);
    }

    free(line_copy);
    return cmd_count;
}

void execute_command(Command *cmd, int input_fd, int output_fd) {
    
    for (int i = 0; cmd->args[i]; i++) {
        char *arg = cmd->args[i];
        size_t len = strlen(arg);
        if (len >= 2 && arg[0] == '"' && arg[len-1] == '"') {
            arg[len-1] = '\0';
            cmd->args[i] = arg + 1;
        }
    }

    pid_t pid = fork();


    if (pid == 0) {

        if (getenv("PATH") == NULL) {
            setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
        }

        if (cmd->input_file) {
            int fd = open(cmd->input_file, O_RDONLY);
            if (fd == -1) {
                perror("open input failed");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        
        if (cmd->output_file) {
            int flags = O_WRONLY | O_CREAT | (cmd->append ? O_APPEND : O_TRUNC);
            int fd = open(cmd->output_file, flags, 0644);
            if (fd == -1) {
                perror("open output failed");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
        
        if (input_fd != STDIN_FILENO) {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }
        
        if (output_fd != STDOUT_FILENO) {
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }
        
        for (int i = 3; i < sysconf(_SC_OPEN_MAX); i++) close(i);
        
        execvp(cmd->args[0], cmd->args);
        perror("execvp failed");
        exit(EXIT_FAILURE);
    }
}

void free_pipeline(Command pipeline[], int cmd_count) {
    for (int i = 0; i < cmd_count; i++) {
        for (int j = 0; pipeline[i].args[j]; j++) {
            free(pipeline[i].args[j]); // Освобождаем копии аргументов
        }
        if (pipeline[i].input_file) {
            free(pipeline[i].input_file); // Освобождаем только если выделено
        }
        if (pipeline[i].output_file) {
            free(pipeline[i].output_file);
        }
    }
}

void execute_pipeline(Command pipeline[], int cmd_count) {
    int fd[2];
    int input_fd = STDIN_FILENO;
    
    for (int i = 0; i < cmd_count; i++) {
        if (i < cmd_count - 1) {
            if (pipe(fd) == -1) {
                perror("pipe failed");
                return;
            }
        }
        
        execute_command(&pipeline[i], input_fd, 
                       (i == cmd_count - 1) ? STDOUT_FILENO : fd[1]);
        
        if (input_fd != STDIN_FILENO) close(input_fd);
        if (i < cmd_count - 1) {
            close(fd[1]);
            input_fd = fd[0];
        }
    }
    
    if (!pipeline[cmd_count-1].background) {
        int status;
        while (waitpid(-1, &status, 0) > 0);
        foreground_mode = 1;
    } else {
        foreground_mode = 0;
    }
}

int main() {
    Command pipeline[MAX_PIPES];
    
    struct sigaction sa = {.sa_handler = sigint_handler, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    
    while (1) {
        printf("myshell> ");
        fflush(stdout);
        
        char *input_line = read_line();
        if (!input_line) break;
        
        if (strlen(input_line) == 0) {
            free(input_line);
            continue;
        }
        
        int cmd_count = parse_pipeline(input_line, pipeline);
        free(input_line);
        
        if (cmd_count == 0) continue;
        
        if (strcmp(pipeline[0].args[0], "exit") == 0) break;
        
        if (execute_builtin(&pipeline[0])) continue;
        
        execute_pipeline(pipeline, cmd_count);
        free_pipeline(pipeline, cmd_count);

    }
    
    for (int i = 0; i < (history_count < MAX_HISTORY ? history_count : MAX_HISTORY); i++) {
        free(history[i]);
    }
    
    return 0;
}