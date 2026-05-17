#include "request_queue.h"

#include "common.h"

#include <animate.h>

volatile sig_atomic_t client_pid = 0;

/*
 * Placement
 */
struct placement {

    struct canvas *canvas;

    struct sprite *sprite;

    int x;

    int y;
};

/*
 * Helper for placement
 */
handle_t create_placement_handle(
    struct canvas *canvas,
    struct sprite *sprite,
    int x,
    int y,
    struct client_info *owner
) {

    struct placement *placement =
        malloc(
            sizeof(
                struct placement
            )
        );

    if (placement == NULL) {
        return 0;
    }

    placement->canvas = canvas;

    placement->sprite = sprite;

    placement->x = x;

    placement->y = y;

    return create_handle(
        placement,
        HANDLE_PLACEMENT,
        owner
    );
}

/*
 * Handle table entry
 */
struct handle_entry {

    void *ptr;

    handle_type_t type;

    struct client_info *owner;

    bool shared;
};

/*
 * Client information
 */
struct client_info {

    pid_t pid;

    bool logged_in;

    char username[64];

    char fifo_c2s[FIFO_NAME_SIZE];

    char fifo_s2c[FIFO_NAME_SIZE];

    handle_t owned_canvases[MAX_OWNED_CANVASES];

    int owned_count;
};

/*
 * Global request queue
 */
struct request_queue request_queue;

/*
 * Global handle table
 */
struct handle_entry handle_table[MAX_HANDLES];

handle_t next_handle = 1;

pthread_mutex_t handle_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Create new handle
 */
handle_t create_handle(
    void *ptr,
    handle_type_t type,
    struct client_info *owner
) {

    pthread_mutex_lock(&handle_mutex);

    if (next_handle >= MAX_HANDLES) {

        pthread_mutex_unlock(&handle_mutex);

        return 0;
    }

    handle_table[next_handle].ptr = ptr;

    handle_table[next_handle].type = type;

    handle_table[next_handle].owner = owner;

    handle_table[next_handle].shared = false;

    handle_t handle = next_handle;

    next_handle++;

    pthread_mutex_unlock(&handle_mutex);

    return handle;
}

/*
 * Queue init
 */
void init_queue() {

    request_queue.front = 0;

    request_queue.rear = 0;

    request_queue.count = 0;

    pthread_mutex_init(
        &request_queue.mutex,
        NULL
    );

    pthread_cond_init(
        &request_queue.not_empty,
        NULL
    );

    pthread_cond_init(
        &request_queue.not_full,
        NULL
    );
}

/*
 * Push request
 */
void push_request(
    struct request request
) {

    pthread_mutex_lock(
        &request_queue.mutex
    );

    while (
        request_queue.count >=
        MAX_QUEUE_SIZE
    ) {

        pthread_cond_wait(
            &request_queue.not_full,
            &request_queue.mutex
        );
    }

    request_queue.items[
        request_queue.rear
    ] = request;

    request_queue.rear =
        (request_queue.rear + 1)
        % MAX_QUEUE_SIZE;

    request_queue.count++;

    pthread_cond_signal(
        &request_queue.not_empty
    );

    pthread_mutex_unlock(
        &request_queue.mutex
    );
}

/*
 * Pop request
 */
struct request pop_request() {

    pthread_mutex_lock(
        &request_queue.mutex
    );

    while (
        request_queue.count == 0
    ) {

        pthread_cond_wait(
            &request_queue.not_empty,
            &request_queue.mutex
        );
    }

    struct request request =
        request_queue.items[
            request_queue.front
        ];

    request_queue.front =
        (request_queue.front + 1)
        % MAX_QUEUE_SIZE;

    request_queue.count--;

    pthread_cond_signal(
        &request_queue.not_full
    );

    pthread_mutex_unlock(
        &request_queue.mutex
    );

    return request;
}

/*
 * Ownership check
 */
bool client_owns_handle(
    struct client_info *client,
    handle_t handle
) {

    if (
        handle == 0 ||
        handle >= next_handle
    ) {

        return false;
    }

    if (
        handle_table[handle].shared
    ) {

        return true;
    }

    return (
        handle_table[handle].owner
        == client
    );
}

/*
 * Get pointer from handle
 */
void *get_pointer(
    handle_t handle,
    handle_type_t expected_type
) {

    if (
        handle == 0 ||
        handle >= next_handle
    ) {

        return NULL;
    }

    if (
        handle_table[handle].type
        != expected_type
    ) {

        return NULL;
    }

    return handle_table[handle].ptr;
}

/*
 * Remove handle
 */
void remove_handle(handle_t handle) {

    if (
        handle == 0 ||
        handle >= next_handle
    ) {

        return;
    }

    handle_table[handle].ptr = NULL;

    handle_table[handle].type = HANDLE_NONE;
}

/*
 * SIGUSR1 handler
 */
void handle_sigusr1(
    int sig,
    siginfo_t *info,
    void *context
) {

    (void)sig;
    (void)context;

    client_pid = info->si_pid;
}

/*
 * Check user from users.txt
 */
int check_user(
    const char *username,
    int *balance
) {

    FILE *file = fopen("users.txt", "r");

    if (file == NULL) {
        return -1;
    }

    char line[256];

    while (
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    ) {

        char file_username[64];

        int file_balance;

        int result =
            sscanf(
                line,
                "%63s %d",
                file_username,
                &file_balance
            );

        if (result == 2) {

            if (
                strcmp(
                    username,
                    file_username
                ) == 0
            ) {

                *balance = file_balance;

                fclose(file);

                return 1;
            }
        }
    }

    fclose(file);

    return 0;
}

/*
 * Send reply to client
 */
void send_reply(
    int fd_write,
    const char *message
) {

    write(
        fd_write,
        message,
        strlen(message)
    );
}

/*
 * Add canvas ownership
 */
void add_owned_canvas(
    struct client_info *client,
    handle_t handle
) {

    if (
        client->owned_count >=
        MAX_OWNED_CANVASES
    ) {

        return;
    }

    client->owned_canvases[
        client->owned_count
    ] = handle;

    client->owned_count++;
}

/*
 * Generate animation file
 */
int generate_animation(
    struct canvas *canvas,
    const char *filename
) {

    return animate_save(
        canvas,
        filename
    );
}

/*
 * Handle client
 */
void *client_handler(void *arg) {

    struct client_info *client =
        (struct client_info *)arg;

    int fd_read =
        open(
            client->fifo_c2s,
            O_RDONLY
        );

    if (fd_read == -1) {

        perror("open c2s");

        free(client);

        return NULL;
    }

    int fd_write =
        open(
            client->fifo_s2c,
            O_WRONLY
        );

    if (fd_write == -1) {

        perror("open s2c");

        close(fd_read);

        free(client);

        return NULL;
    }

    printf(
        "Client connected: %d\n",
        client->pid
    );

    char buffer[BUFFER_SIZE];

    while (1) {

        ssize_t bytes_read =
            read(
                fd_read,
                buffer,
                BUFFER_SIZE - 1
            );

        if (bytes_read <= 0) {
            break;
        }

        buffer[bytes_read] = '\0';

        printf(
            "Received: %s",
            buffer
        );

        struct request request;

        request.client = client;

        request.fd_write = fd_write;

        strcpy(
            request.command,
            buffer
        );

        push_request(request);
    }

    /*
     * Cleanup owned handles
     */
    for (
        handle_t i = 1;
        i < next_handle;
        i++
    ) {

        if (
            handle_table[i].owner
            == client
        ) {

            if (
                handle_table[i].type
                == HANDLE_CANVAS
            ) {

                struct canvas *canvas =
                    handle_table[i].ptr;

                if (canvas != NULL) {

                    animate_destroy_canvas(
                        canvas
                    );
                }
            }

            else if (
                handle_table[i].type
                == HANDLE_PLACEMENT
            ) {

                free(
                    handle_table[i].ptr
                );
            }

            remove_handle(i);
        }
    }

    close(fd_read);
    close(fd_write);

    unlink(client->fifo_c2s);

    unlink(client->fifo_s2c);

    printf(
        "Client disconnected: %d\n",
        client->pid
    );

    free(client);

    return NULL;
}

/*
 * Handle request
 */
void process_request(
    struct request request
) {
    /*
     * Login
     */
    if (
        strncmp(
            request.command,
            "Login ",
            6
        ) == 0
    ) {

        char username[64];

        sscanf(
            request.command + 6,
            "%63s",
            username
        );

        int balance;

        int result =
            check_user(
                username,
                &balance
            );

        if (result == 1) {

            if (balance > 0) {

                client->logged_in = true;

                strcpy(
                    client->username,
                    username
                );

                char reply[BUFFER_SIZE];

                snprintf(
                    reply,
                    BUFFER_SIZE,
                    "LOGIN_OK %d\n",
                    balance
                );

                send_reply(
                    fd_write,
                    reply
                );
            }
            else {

                send_reply(
                    fd_write,
                    "Reject BALANCE\n"
                );

                sleep(1);

                break;
            }
        }
        else {

            send_reply(
                fd_write,
                "Reject UNAUTHORISED\n"
            );

            sleep(1);

            break;
        }
    }

    /*
     * create_canvas
     */
    else if (
        strncmp(
            request.command,
            "create_canvas ",
            14
        ) == 0
    ) {

        size_t width;
        size_t height;

        int result =
            sscanf(
                request.command + 14,
                "%zu %zu",
                &width,
                &height
            );

        if (result != 2) {

            send_reply(
                fd_write,
                "-1\n"
            );

            continue;
        }

        struct canvas *canvas =
            animate_create_canvas(
                width,
                height
            );

        if (canvas == NULL) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        handle_t handle =
            create_handle(
                canvas,
                HANDLE_CANVAS
            );

        if (handle == 0) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        add_owned_canvas(
            client,
            handle
        );

        char reply[BUFFER_SIZE];

        snprintf(
            reply,
            BUFFER_SIZE,
            "0 %lu\n",
            handle
        );

        send_reply(
            fd_write,
            reply
        );
    }

    /*
     * create_rectangle
     */
    else if (
        strncmp(
            request.command,
            "create_rectangle ",
            17
        ) == 0
    ) {

        int red;
        int green;
        int blue;

        size_t width;
        size_t height;

        int result =
            sscanf(
                request.command + 17,
                "%d %d %d %zu %zu",
                &red,
                &green,
                &blue,
                &width,
                &height
            );

        if (result != 5) {

            send_reply(
                fd_write,
                "-1\n"
            );

            continue;
        }

        struct sprite *sprite =
            animate_create_rectangle(
                red,
                green,
                blue,
                width,
                height
            );

        if (sprite == NULL) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        handle_t handle =
            create_handle(
                sprite,
                HANDLE_SPRITE
            );

        char reply[BUFFER_SIZE];

        snprintf(
            reply,
            BUFFER_SIZE,
            "0 %lu\n",
            handle
        );

        send_reply(
            fd_write,
            reply
        );
    }

    /*
     * create_circle
     */
    else if (
        strncmp(
            request.command,
            "create_circle ",
            14
        ) == 0
    ) {

        int red;
        int green;
        int blue;

        size_t radius;

        int result =
            sscanf(
                request.command + 14,
                "%d %d %d %zu",
                &red,
                &green,
                &blue,
                &radius
            );

        if (result != 4) {

            send_reply(
                fd_write,
                "-1\n"
            );

            continue;
        }

        struct sprite *sprite =
            animate_create_circle(
                red,
                green,
                blue,
                radius
            );

        if (sprite == NULL) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        handle_t handle =
            create_handle(
                sprite,
                HANDLE_SPRITE
            );

        char reply[BUFFER_SIZE];

        snprintf(
            reply,
            BUFFER_SIZE,
            "0 %lu\n",
            handle
        );

        send_reply(
            fd_write,
            reply
        );
    }

    /*
     * add_to_canvas
     */
    else if (
        strncmp(
            request.command,
            "add_to_canvas ",
            14
        ) == 0
    ) {

        handle_t canvas_handle;
        handle_t sprite_handle;

        int x;
        int y;

        int result =
            sscanf(
                request.command + 14,
                "%lu %lu %d %d",
                &canvas_handle,
                &sprite_handle,
                &x,
                &y
            );

        if (result != 4) {

            send_reply(
                fd_write,
                "-1\n"
            );

            continue;
        }

        struct canvas *canvas =
            get_pointer(
                canvas_handle,
                HANDLE_CANVAS
            );

        struct sprite *sprite =
            get_pointer(
                sprite_handle,
                HANDLE_SPRITE
            );

        if (
            canvas == NULL ||
            sprite == NULL
        ) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        int api_result =
            animate_add_sprite(
                canvas,
                sprite,
                x,
                y
            );

        if (api_result != 0) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        handle_t placement_handle =
            create_placement_handle(
                canvas,
                sprite,
                x,
                y,
                request.client
            );

        if (placement_handle == 0) {

            send_reply(
                request.fd_write,
                "-2\n"
            );

            return;
        }

        char reply[BUFFER_SIZE];

        snprintf(
            reply,
            BUFFER_SIZE,
            "0 %lu\n",
            placement_handle
        );

        send_reply(
            request.fd_write,
            reply
        );
    }

    /*
     * move
     */
    else if (
        strncmp(
            request.command,
            "move ",
            5
        ) == 0
    ) {

        handle_t placement_handle;

        int new_x;
        int new_y;

        int result =
            sscanf(
                request.command + 5,
                "%lu %d %d",
                &placement_handle,
                &new_x,
                &new_y
            );

        if (result != 3) {

            send_reply(
                request.fd_write,
                "-1\n"
            );

            return;
        }

        /*
         * Ownership
         */
        if (
            !client_owns_handle(
                request.client,
                placement_handle
            )
        ) {

            send_reply(
                request.fd_write,
                "-2\n"
            );

            return;
        }

        struct placement *placement =
            get_pointer(
                placement_handle,
                HANDLE_PLACEMENT
            );

        if (placement == NULL) {

            send_reply(
                request.fd_write,
                "-2\n"
            );

            return;
        }

        int api_result =
            animate_move_sprite(
                placement->canvas,
                placement->sprite,
                new_x,
                new_y
            );

        if (api_result != 0) {

            send_reply(
                request.fd_write,
                "-2\n"
            );

            return;
        }

        placement->x = new_x;

        placement->y = new_y;

        send_reply(
            request.fd_write,
            "0\n"
        );
    }

    /*
     * remove_sprite
     */
    else if (
        strncmp(
            request.command,
            "remove_sprite ",
            14
        ) == 0
    ) {

        handle_t canvas_handle;
        handle_t sprite_handle;

        int result =
            sscanf(
                request.command + 14,
                "%lu %lu",
                &canvas_handle,
                &sprite_handle
            );

        if (result != 2) {

            send_reply(
                fd_write,
                "-1\n"
            );

            continue;
        }

        struct canvas *canvas =
            get_pointer(
                canvas_handle,
                HANDLE_CANVAS
            );

        struct sprite *sprite =
            get_pointer(
                sprite_handle,
                HANDLE_SPRITE
            );

        if (
            canvas == NULL ||
            sprite == NULL
        ) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        int api_result =
            animate_remove_sprite(
                canvas,
                sprite
            );

        if (api_result != 0) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        send_reply(
            fd_write,
            "0\n"
        );
    }

    /*
     * generate
     */
    else if (
        strncmp(
            request.command,
            "generate ",
            9
        ) == 0
    ) {

        handle_t canvas_handle;

        char filename[256];

        int result =
            sscanf(
                request.command + 9,
                "%lu %255s",
                &canvas_handle,
                filename
            );

        if (result != 2) {

            send_reply(
                fd_write,
                "-1\n"
            );

            continue;
        }

        struct canvas *canvas =
            get_pointer(
                canvas_handle,
                HANDLE_CANVAS
            );

        if (canvas == NULL) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        int api_result =
            generate_animation(
                canvas,
                filename
            );

        if (api_result != 0) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        send_reply(
            fd_write,
            "0\n"
        );
    }

    /*
     * share_canvas
     */
    else if (
        strncmp(
            request.command,
            "share_canvas ",
            13
        ) == 0
    ) {

        handle_t canvas_handle;

        int result =
            sscanf(
                request.command + 13,
                "%lu",
                &canvas_handle
            );

        if (result != 1) {

            send_reply(
                request.fd_write,
                "-1\n"
            );

            return;
        }

        /*
         * Ownership check
         */
        if (
            !client_owns_handle(
                request.client,
                canvas_handle
            )
        ) {

            send_reply(
                request.fd_write,
                "-2\n"
            );

            return;
        }

        /*
         * Must be canvas
         */
        if (
            handle_table[canvas_handle].type
            != HANDLE_CANVAS
        ) {

            send_reply(
                request.fd_write,
                "-2\n"
            );

            return;
        }

        handle_table[
            canvas_handle
        ].shared = true;

        send_reply(
            request.fd_write,
            "0\n"
        );
    }

    /*
     * destroy_canvas
     */
    else if (
        strncmp(
            request.command,
            "destroy_canvas ",
            15
        ) == 0
    ) {

        handle_t canvas_handle;

        int result =
            sscanf(
                request.command + 15,
                "%lu",
                &canvas_handle
            );

        if (result != 1) {

            send_reply(
                fd_write,
                "-1\n"
            );

            continue;
        }

        struct canvas *canvas =
            get_pointer(
                canvas_handle,
                HANDLE_CANVAS
            );

        if (canvas == NULL) {

            send_reply(
                fd_write,
                "-2\n"
            );

            continue;
        }

        animate_destroy_canvas(canvas);

        remove_handle(canvas_handle);

        send_reply(
            fd_write,
            "0\n"
        );
    }
    
    /*
     * disconnect
     */
    else if (
        strcmp(
            request.command,
            "disconnect\n"
        ) == 0
    ) {

        send_reply(
            request.fd_write,
            "BYE\n"
        );

        close(request.fd_write);

        pthread_exit(NULL);
    }

    else {

        send_reply(
            fd_write,
            "Unknown command\n"
        );
    }
}

void *worker_thread(
    void *arg
) {

    (void)arg;

    while (1) {

        struct request request =
            pop_request();

        process_request(request);
    }

    return NULL;
}

int main(int argc, char *argv[]) {

    init_queue();

    if (argc != 2) {

        fprintf(
            stderr,
            "Usage: %s <threadpool size>\n",
            argv[0]
        );

        return 1;
    }

    printf(
        "Server PID: %d\n",
        getpid()
    );

    fflush(stdout);

    struct sigaction sa;

    sa.sa_sigaction = handle_sigusr1;

    sigemptyset(&sa.sa_mask);

    sa.sa_flags = SA_SIGINFO;

    if (
        sigaction(
            SIGUSR1,
            &sa,
            NULL
        ) == -1
    ) {

        perror("sigaction");

        return 1;
    }
    
    int threadpool_size =
        atoi(argv[1]);

    pthread_t workers[
        threadpool_size
    ];

    for (
        int i = 0;
        i < threadpool_size;
        i++
    ) {

        pthread_create(
            &workers[i],
            NULL,
            worker_thread,
            NULL
        );
    }

    while (1) {

        if (client_pid != 0) {

            pid_t pid = client_pid;

            client_pid = 0;

            struct client_info *client =
                malloc(
                    sizeof(
                        struct client_info
                    )
                );

            if (client == NULL) {

                perror("malloc");

                continue;
            }

            memset(
                client,
                0,
                sizeof(struct client_info)
            );

            client->pid = pid;

            snprintf(
                client->fifo_c2s,
                FIFO_NAME_SIZE,
                "FIFO_C2S_%d",
                pid
            );

            snprintf(
                client->fifo_s2c,
                FIFO_NAME_SIZE,
                "FIFO_S2C_%d",
                pid
            );

            unlink(client->fifo_c2s);

            unlink(client->fifo_s2c);

            if (
                mkfifo(
                    client->fifo_c2s,
                    0666
                ) == -1
            ) {

                perror("mkfifo c2s");

                free(client);

                continue;
            }

            if (
                mkfifo(
                    client->fifo_s2c,
                    0666
                ) == -1
            ) {

                perror("mkfifo s2c");

                unlink(client->fifo_c2s);

                free(client);

                continue;
            }

            kill(pid, SIGUSR2);

            pthread_t thread;

            if (
                pthread_create(
                    &thread,
                    NULL,
                    client_handler,
                    client
                ) != 0
            ) {

                perror("pthread_create");

                unlink(client->fifo_c2s);

                unlink(client->fifo_s2c);

                free(client);

                continue;
            }

            pthread_detach(thread);
        }

        usleep(10000);
    }

    return 0;
}
close(fd_read);
close(fd_write);