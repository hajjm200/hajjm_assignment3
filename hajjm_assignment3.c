#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#define MAX_LINE_LEN 2048
#define MAX_ARGS 512
#define MAX_BG_PROCS 100

int lastStatus = 0;
int foregroundOnlyMode = 0;
pid_t bgPIDs[MAX_BG_PROCS];
int bgCount = 0;

void handle_SIGTSTP(int signo) {
    char* enterMsg = "\nEntering foreground-only mode (& is now ignored)\n";
    char* exitMsg = "\nExiting foreground-only mode\n";
    if (foregroundOnlyMode == 0) {
        write(STDOUT_FILENO, enterMsg, 50);
        foregroundOnlyMode = 1;
    } else {
        write(STDOUT_FILENO, exitMsg, 30);
        foregroundOnlyMode = 0;
    }
}

void setupSignals() {
    struct sigaction SIGINT_action = {0}, SIGTSTP_action = {0};
    SIGINT_action.sa_handler = SIG_IGN;
    sigaction(SIGINT, &SIGINT_action, NULL);

    SIGTSTP_action.sa_handler = handle_SIGTSTP;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}

void checkBackgroundProcesses() {
    for (int i = 0; i < bgCount; i++) {
        int status;
        pid_t donePID = waitpid(bgPIDs[i], &status, WNOHANG);
        if (donePID > 0) {
            if (WIFEXITED(status)) {
                printf("background pid %d is done: exit value %d\n", donePID, WEXITSTATUS(status));
            } else {
                printf("background pid %d is done: terminated by signal %d\n", donePID, WTERMSIG(status));
            }
            fflush(stdout);
            for (int j = i; j < bgCount - 1; j++) {
                bgPIDs[j] = bgPIDs[j + 1];
            }
            bgCount--;
            i--;
        }
    }
}

void changeDirectory(char* path) {
    if (path == NULL) path = getenv("HOME");
    if (chdir(path) != 0) perror("cd");
}

void printStatus() {
    printf("exit value %d\n", lastStatus);
}

void executeCommand(char* args[], char* inputFile, char* outputFile, int background) {
    pid_t spawnPid = fork();
    int childStatus;

    switch (spawnPid) {
        case -1:
            perror("fork() failed");
            exit(1);
            break;

        case 0: {
            if (!background || foregroundOnlyMode) {
                struct sigaction SIGINT_action = {0};
                SIGINT_action.sa_handler = SIG_DFL;
                sigaction(SIGINT, &SIGINT_action, NULL);
            }

            if (inputFile) {
                int inFD = open(inputFile, O_RDONLY);
                if (inFD == -1) {
                    perror("cannot open input file");
                    exit(1);
                }
                dup2(inFD, 0);
                close(inFD);
            } else if (background && !foregroundOnlyMode) {
                int devNull = open("/dev/null", O_RDONLY);
                dup2(devNull, 0);
                close(devNull);
            }

            if (outputFile) {
                int outFD = open(outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (outFD == -1) {
                    perror("cannot open output file");
                    exit(1);
                }
                dup2(outFD, 1);
                close(outFD);
            } else if (background && !foregroundOnlyMode) {
                int devNull = open("/dev/null", O_WRONLY);
                dup2(devNull, 1);
                close(devNull);
            }

            execvp(args[0], args);
            perror(args[0]);
            exit(1);
            break;
        }

        default:
            if (background && !foregroundOnlyMode) {
                printf("background pid is %d\n", spawnPid);
                fflush(stdout);
                bgPIDs[bgCount++] = spawnPid;
            } else {
                waitpid(spawnPid, &childStatus, 0);
                if (WIFEXITED(childStatus)) {
                    lastStatus = WEXITSTATUS(childStatus);
                } else if (WIFSIGNALED(childStatus)) {
                    lastStatus = WTERMSIG(childStatus);
                    printf("terminated by signal %d\n", lastStatus);
                    fflush(stdout);
                }
            }
            break;
    }
}


char* expandVariable(const char* input) {
    char* result = malloc(2048); // MAX_LINE_LEN
    result[0] = '\0';

    pid_t shellPid = getpid();
    char pidStr[20];
    sprintf(pidStr, "%d", shellPid);

    const char* curr = input;
    while (*curr != '\0') {
        if (*curr == '$' && *(curr + 1) == '$') {
            strcat(result, pidStr);
            curr += 2;
        } else {
            strncat(result, curr, 1);
            curr++;
        }
    }

    return result;
}


int main() {
    setupSignals();
    char* line = NULL;
    size_t bufsize = 0;

    while (1) {
        checkBackgroundProcesses();
        printf(": ");
        fflush(stdout);

        ssize_t charsRead = getline(&line, &bufsize, stdin);
        if (charsRead == -1) {
            clearerr(stdin);
            continue;
        }

        if (line[charsRead - 1] == '\n') line[charsRead - 1] = '\0';
        if (line[0] == '\0' || line[0] == '#') continue;

        char* args[MAX_ARGS];
        int argCount = 0;
        char* inputFile = NULL;
        char* outputFile = NULL;
        int background = 0;

        char* token = strtok(line, " ");
        while (token != NULL && argCount < MAX_ARGS - 1) {
            if (strcmp(token, "<") == 0) {
                token = strtok(NULL, " ");
                inputFile = token;
            } else if (strcmp(token, ">") == 0) {
                token = strtok(NULL, " ");
                outputFile = token;
            } else if (strcmp(token, "&") == 0 && strtok(NULL, " ") == NULL) {
                background = 1;
                break;
            } else {
                args[argCount++] = token;
            }
            token = strtok(NULL, " ");
        }
        args[argCount] = NULL;

        if (args[0] == NULL) continue;

        if (strcmp(args[0], "exit") == 0) {
            for (int i = 0; i < bgCount; i++) kill(bgPIDs[i], SIGTERM);
            break;
        }

        if (strcmp(args[0], "cd") == 0) {
            changeDirectory(args[1]);
            continue;
        }

        if (strcmp(args[0], "status") == 0) {
            printStatus();
            continue;
        }

        executeCommand(args, inputFile, outputFile, background);
    }

    free(line);
    return 0;
}
