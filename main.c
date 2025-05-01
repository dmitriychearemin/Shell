#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

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

int parse_pipeline(char *line, Command pipeline[]) {
    int cmd_count = 0;
    char *token = strtok(line, "|");
    
    while (token != NULL && cmd_count < MAX_PIPES) {
        if (strlen(token) == 0) {
            token = strtok(NULL, "|");
            continue;
        }

        pipeline[cmd_count].input_file = NULL;
        pipeline[cmd_count].output_file = NULL;
        pipeline[cmd_count].append = 0;
        pipeline[cmd_count].background = 0;

        char *ptr = strtok(token, " \t\n");
        pipeline[cmd_count].args[0] = ptr;
        int i = 1;
        
        while ((ptr = strtok(NULL, " \t\n")) != NULL) {
            if (strcmp(ptr, ">") == 0) {
                pipeline[cmd_count].output_file = strtok(NULL, " \t\n");
                pipeline[cmd_count].append = 0;
                break;
            } else if (strcmp(ptr, ">>") == 0) {
                pipeline[cmd_count].output_file = strtok(NULL, " \t\n");
                pipeline[cmd_count].append = 1;
                break;
            } else if (strcmp(ptr, "<") == 0) {
                pipeline[cmd_count].input_file = strtok(NULL, " \t\n");
            } else if (strcmp(ptr, "&") == 0) {
                pipeline[cmd_count].background = 1;
            } else {
                pipeline[cmd_count].args[i++] = ptr;
            }
        }
        pipeline[cmd_count].args[i] = NULL;
        cmd_count++;
        token = strtok(NULL, "|");
    }
    return cmd_count;
}

void execute_command(Command *cmd, int input_fd, int output_fd) {
    pid_t pid = fork();
    
    if (pid == 0) {
        // Перенаправление ввода
        if (cmd->input_file) {
            int fd = open(cmd->input_file, O_RDONLY);
            if (fd == -1) {
                perror("open input failed");
                exit(EXIT_FAILURE);
            }
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        
        // Перенаправление вывода
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
        
        // Обработка конвейера
        if (input_fd != STDIN_FILENO) {
            dup2(input_fd, STDIN_FILENO);
            close(input_fd);
        }
        
        if (output_fd != STDOUT_FILENO) {
            dup2(output_fd, STDOUT_FILENO);
            close(output_fd);
        }
        
        // Закрываем все ненужные дескрипторы
        for (int i = 3; i < sysconf(_SC_OPEN_MAX); i++) close(i);
        
        execvp(cmd->args[0], cmd->args);
        perror("execvp failed");
        exit(EXIT_FAILURE);
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
    
    // Ожидание завершения
    if (!pipeline[cmd_count-1].background) {
        int status;
        while (waitpid(-1, &status, 0) > 0);
        foreground_mode = 1;
    } else {
        foreground_mode = 0;
    }
}

int main() {
    char line[MAX_CMD_LEN];
    Command pipeline[MAX_PIPES];
    
    struct sigaction sa = {.sa_handler = sigint_handler, .sa_flags = SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    
    while (1) {
        printf("myshell> ");
        fflush(stdout);
        
        if (!fgets(line, sizeof(line), stdin)) break;
        
        line[strcspn(line, "\n")] = '\0';
        add_to_history(line);
        
        int cmd_count = parse_pipeline(line, pipeline);
        if (cmd_count == 0) continue;
        
        if (strcmp(pipeline[0].args[0], "exit") == 0) break;
        
        if (execute_builtin(&pipeline[0])) continue;
        
        execute_pipeline(pipeline, cmd_count);
    }
    
    // Очистка истории
    for (int i = 0; i < (history_count < MAX_HISTORY ? history_count : MAX_HISTORY); i++) {
        free(history[i]);
    }
    
    return 0;
}