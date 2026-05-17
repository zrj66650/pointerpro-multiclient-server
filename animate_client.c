#include "common.h"

volatile sig_atomic_t connected = 0;

void handle_sigusr2(int sig) {

    (void)sig;

    connected = 1;
}

int main(int argc, char *argv[]) {

    if (argc != 2) {

        fprintf(
            stderr,
            "Usage: %s <server pid>\n",
            argv[0]
        );

        return 1;
    }

    pid_t server_pid = atoi(argv[1]);

    signal(
        SIGUSR2,
        handle_sigusr2
    );

    /*
     * Connect to server
     */
    kill(
        server_pid,
        SIGUSR1
    );

    int wait_count = 0;

    while (!connected) {

        usleep(10000);

        wait_count++;

        if (wait_count > 100) {

            printf(
                "Connection timeout\n"
            );

            return 1;
        }
    }

    char fifo_c2s[FIFO_NAME_SIZE];

    char fifo_s2c[FIFO_NAME_SIZE];

    snprintf(
        fifo_c2s,
        FIFO_NAME_SIZE,
        "FIFO_C2S_%d",
        getpid()
    );

    snprintf(
        fifo_s2c,
        FIFO_NAME_SIZE,
        "FIFO_S2C_%d",
        getpid()
    );

    int fd_write =
        open(
            fifo_c2s,
            O_WRONLY
        );

    if (fd_write == -1) {

        perror("open c2s");

        return 1;
    }

    int fd_read =
        open(
            fifo_s2c,
            O_RDONLY
        );

    if (fd_read == -1) {

        perror("open s2c");

        close(fd_write);

        return 1;
    }

    printf(
        "Connected to server\n"
    );

    bool logged_in = false;

    char input[BUFFER_SIZE];

    while (1) {

        printf("> ");

        fflush(stdout);

        if (
            fgets(
                input,
                BUFFER_SIZE,
                stdin
            ) == NULL
        ) {

            break;
        }

        /*
         * Must login first
         */
        if (
            !logged_in &&
            strncmp(
                input,
                "Login ",
                6
            ) != 0
        ) {

            printf(
                "Not logged in\n"
            );

            continue;
        }

        /*
         * Send request
         */
        write(
            fd_write,
            input,
            strlen(input)
        );

        /*
         * Read reply
         */
        char reply[BUFFER_SIZE];

        ssize_t bytes_read =
            read(
                fd_read,
                reply,
                BUFFER_SIZE - 1
            );

        if (bytes_read <= 0) {

            printf(
                "Server disconnected\n"
            );

            break;
        }

        reply[bytes_read] = '\0';

        /*
         * Login success
         */
        if (
            strncmp(
                reply,
                "LOGIN_OK ",
                9
            ) == 0
        ) {

            int balance;

            sscanf(
                reply + 9,
                "%d",
                &balance
            );

            printf(
                "Welcome. Balance = %d\n",
                balance
            );

            logged_in = true;
        }

        /*
         * Disconnect
         */
        else if (
            strcmp(
                reply,
                "BYE\n"
            ) == 0
        ) {

            printf(
                "Disconnected\n"
            );

            break;
        }

        /*
         * Reject
         */
        else if (
            strncmp(
                reply,
                "Reject",
                6
            ) == 0
        ) {

            printf(
                "%s",
                reply
            );
        }

        /*
         * Parse error
         */
        else if (
            strcmp(
                reply,
                "-1\n"
            ) == 0
        ) {

            printf(
                "Parse error\n"
            );
        }

        /*
         * API error
         */
        else if (
            strcmp(
                reply,
                "-2\n"
            ) == 0
        ) {

            printf(
                "API error\n"
            );
        }

        /*
         * Success
         */
        else if (
            reply[0] == '0'
        ) {

            printf(
                "RPC Success: %s",
                reply
            );
        }

        else {

            printf(
                "Unknown response: %s",
                reply
            );
        }
    }

    close(fd_write);

    close(fd_read);

    return 0;
}