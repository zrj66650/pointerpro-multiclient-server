#define _POSIX_C_SOURCE 200809L
#include "common.h"

volatile sig_atomic_t connected = 0;

/* SIGUSR2 handler - called when server confirms connection */
void handle_sigusr2(int sig) {
    (void)sig;
    connected = 1;
}

/* Send command to server and read reply */
char *send_command(int fd_write, int fd_read, const char *cmd) {
    write(fd_write, cmd, strlen(cmd));

    char *reply = malloc(BUFFER_SIZE);
    if (!reply) return NULL;

    ssize_t bytes_read = read(fd_read, reply, BUFFER_SIZE - 1);
    if (bytes_read <= 0) {
        free(reply);
        return NULL;
    }
    reply[bytes_read] = '\0';
    return reply;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <server pid>\n", argv[0]);
        return 1;
    }

    pid_t server_pid = atoi(argv[1]);
    pid_t client_pid = getpid();

    /* Set up SIGUSR2 handler */
    struct sigaction sa;
    sa.sa_handler = handle_sigusr2;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR2, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    /* Send SIGUSR1 to initiate connection */
    if (kill(server_pid, SIGUSR1) == -1) {
        perror("kill");
        return 1;
    }

    /* Wait for SIGUSR2 with timeout (approx 1 second) */
    int wait_count = 0;
    while (!connected && wait_count < 100) {
        usleep(10000);
        wait_count++;
    }
    if (!connected) {
        printf("Connection timeout\n");
        return 1;
    }

    /* Build FIFO names */
    char fifo_c2s[FIFO_NAME_SIZE], fifo_s2c[FIFO_NAME_SIZE];
    snprintf(fifo_c2s, FIFO_NAME_SIZE, "FIFO_C2S_%d", client_pid);
    snprintf(fifo_s2c, FIFO_NAME_SIZE, "FIFO_S2C_%d", client_pid);

    /* Open FIFOs (write first, then read) */
    int fd_write = open(fifo_c2s, O_WRONLY);
    if (fd_write == -1) {
        perror("open c2s");
        return 1;
    }

    int fd_read = open(fifo_s2c, O_RDONLY);
    if (fd_read == -1) {
        perror("open s2c");
        close(fd_write);
        return 1;
    }

    bool logged_in = false;
    char username[64] = {0};
    char input[BUFFER_SIZE];

    /* Main command loop */
    while (1) {
        printf("> ");
        fflush(stdout);

        if (fgets(input, BUFFER_SIZE, stdin) == NULL)
            break;

        /* Remove trailing newline */
        size_t len = strlen(input);
        if (len > 0 && input[len-1] == '\n')
            input[len-1] = '\0';

        if (strlen(input) == 0)
            continue;

        /* Check login state (except for Login command) */
        if (!logged_in && strncmp(input, "Login", 5) != 0) {
            printf("Not logged in\n");
            continue;
        }

        /* Store username for welcome message */
        if (strncmp(input, "Login ", 6) == 0)
            strcpy(username, input + 6);

        /* Send command with required newline */
        char cmd_with_newline[BUFFER_SIZE];
        snprintf(cmd_with_newline, BUFFER_SIZE, "%s\n", input);
        char *reply = send_command(fd_write, fd_read, cmd_with_newline);

        if (!reply) {
            printf("Server disconnected\n");
            break;
        }

        /* Remove trailing newline from reply for display */
        char *newline = strchr(reply, '\n');
        if (newline) *newline = '\0';

        /* Handle different response types */
        if (strncmp(reply, "Reject", 6) == 0) {
            printf("%s\n", reply);
            free(reply);
            break;
        } else if (strcmp(reply, "BYE") == 0) {
            printf("Disconnected\n");
            free(reply);
            close(fd_write);
            close(fd_read);
            return 0;
        } else if (reply[0] == '-' && strlen(reply) >= 2 &&
                   (reply[1] == '1' || reply[1] == '2' || reply[1] == '3')) {
            switch (reply[1]) {
                case '1': printf("RPC Failed\n"); break;
                case '2': printf("Value error\n"); break;
                case '3': printf("Internal error\n"); break;
                default:  printf("Unknown error: %s\n", reply); break;
            }
        } else if (strncmp(reply, "0", 1) == 0) {
            if (reply[1] == ' ' && reply[2] != '\0') {
                printf("Success\n");
                printf("Handle: %s\n", reply + 2);
            } else if (reply[1] == '\0') {
                printf("Success\n");
            } else {
                printf("%s\n", reply);
            }
        } else if (reply[0] >= '0' && reply[0] <= '9') {
            /* Balance response from login */
            int balance = atoi(reply);
            printf("Welcome %s. Your balance is %d\n", username, balance);
            logged_in = true;
        } else {
            printf("Response: %s\n", reply);
        }

        free(reply);
    }

    /* Send disconnect if logged in and not already closed */
    if (logged_in) {
        send_command(fd_write, fd_read, "Disconnect\n");
    }

    close(fd_write);
    close(fd_read);

    return 0;
}