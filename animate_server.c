#define _POSIX_C_SOURCE 200809L
#include "request_queue.h"
#include "common.h"
#include "animate.h"
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>

volatile sig_atomic_t client_pid = 0;


/* Placement structure - matches P1's sprite_placement */
struct placement_wrapper {
    struct canvas *canvas;
    struct sprite *sprite;
    struct sprite_placement *placement_ptr;
    ssize_t x;
    ssize_t y;
};


/* Handle table entry */
struct handle_entry {
    void *ptr;
    handle_type_t type;
    struct client_info *owner;
    bool shared;
    char shared_users[MAX_SHARED_USERS][64];
    int shared_count;
};


/* Client information */
struct client_info {
    pid_t pid;
    bool logged_in;
    char username[64];
    char fifo_c2s[FIFO_NAME_SIZE];
    char fifo_s2c[FIFO_NAME_SIZE];
    handle_t owned_canvases[MAX_OWNED_CANVASES];
    int owned_count;
    bool should_disconnect;
};


/* Global variables */
struct request_queue request_queue;
struct handle_entry handle_table[MAX_HANDLES];
handle_t next_handle = 1;
pthread_mutex_t handle_mutex = PTHREAD_MUTEX_INITIALIZER;


/* Global client list for monitoring */
struct client_info *all_clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t client_list_mutex = PTHREAD_MUTEX_INITIALIZER;


/* Function declarations */
handle_t create_handle(void *ptr, handle_type_t type, struct client_info *owner);
void *get_pointer(handle_t handle, handle_type_t expected_type);
bool client_owns_handle(struct client_info *client, handle_t handle);
void remove_handle(handle_t handle);
void add_owned_canvas(struct client_info *client, handle_t handle);
void send_reply(int fd_write, const char *message);
int check_user(const char *username, int *balance);
handle_t create_placement_wrapper_handle(struct canvas *canvas, struct sprite *sprite,
                                          ssize_t x, ssize_t y, struct client_info *owner,
                                          struct sprite_placement *placement_ptr);
int generate_animation(struct canvas *canvas, const char *filename,
                       int start_frame, int end_frame, int frame_rate, int fd_write);
void add_client_to_list(struct client_info *client);
void remove_client_from_list(struct client_info *client);
void check_client_alive(struct client_info *client);
bool is_canvas_shared_with_user(handle_t canvas_handle, const char *username);
void add_shared_user(handle_t canvas_handle, const char *username);
void remove_shared_user(handle_t canvas_handle, const char *username);
bool is_canvas_in_use_by_others(handle_t canvas_handle, struct client_info *exclude_client);


/* Create new handle */
handle_t create_handle(void *ptr, handle_type_t type, struct client_info *owner) {
    pthread_mutex_lock(&handle_mutex);

    if (next_handle >= MAX_HANDLES) {
        pthread_mutex_unlock(&handle_mutex);
        return 0;
    }

    handle_table[next_handle].ptr = ptr;
    handle_table[next_handle].type = type;
    handle_table[next_handle].owner = owner;
    handle_table[next_handle].shared = false;
    handle_table[next_handle].shared_count = 0;
    memset(handle_table[next_handle].shared_users, 0, sizeof(handle_table[next_handle].shared_users));

    handle_t handle = next_handle;
    next_handle++;

    pthread_mutex_unlock(&handle_mutex);
    return handle;
}


/* Create placement wrapper handle */
handle_t create_placement_wrapper_handle(struct canvas *canvas, struct sprite *sprite,
                                          ssize_t x, ssize_t y, struct client_info *owner,
                                          struct sprite_placement *placement_ptr) {
    struct placement_wrapper *pw = malloc(sizeof(struct placement_wrapper));

    if (pw == NULL) return 0;

    pw->canvas = canvas;
    pw->sprite = sprite;
    pw->placement_ptr = placement_ptr;
    pw->x = x;
    pw->y = y;

    return create_handle(pw, HANDLE_PLACEMENT, owner);
}


/* Get pointer from handle */
void *get_pointer(handle_t handle, handle_type_t expected_type) {
    if (handle == 0 || handle >= next_handle) return NULL;
    if (handle_table[handle].type != expected_type) return NULL;

    return handle_table[handle].ptr;
}


/* Check if client owns a handle (or has shared access) */
bool client_owns_handle(struct client_info *client, handle_t handle) {
    if (handle == 0 || handle >= next_handle) return false;

    struct handle_entry *entry = &handle_table[handle];

    if (entry->shared) {
        for (int i = 0; i < entry->shared_count; i++) {
            if (strcmp(entry->shared_users[i], client->username) == 0) {
                return true;
            }
        }
    }

    return (entry->owner == client);
}


/* Check if canvas is shared with a specific user */
bool is_canvas_shared_with_user(handle_t canvas_handle, const char *username) {
    if (canvas_handle == 0 || canvas_handle >= next_handle) return false;

    struct handle_entry *entry = &handle_table[canvas_handle];

    if (!entry->shared) return false;

    for (int i = 0; i < entry->shared_count; i++) {
        if (strcmp(entry->shared_users[i], username) == 0) {
            return true;
        }
    }

    return false;
}


/* Add shared user to canvas */
void add_shared_user(handle_t canvas_handle, const char *username) {
    if (canvas_handle == 0 || canvas_handle >= next_handle) return;

    struct handle_entry *entry = &handle_table[canvas_handle];

    if (entry->shared_count >= MAX_SHARED_USERS) return;

    for (int i = 0; i < entry->shared_count; i++) {
        if (strcmp(entry->shared_users[i], username) == 0) return;
    }

    strcpy(entry->shared_users[entry->shared_count], username);
    entry->shared_count++;
    entry->shared = true;
}


/* Remove shared user from canvas */
void remove_shared_user(handle_t canvas_handle, const char *username) {
    if (canvas_handle == 0 || canvas_handle >= next_handle) return;

    struct handle_entry *entry = &handle_table[canvas_handle];

    for (int i = 0; i < entry->shared_count; i++) {
        if (strcmp(entry->shared_users[i], username) == 0) {
            for (int j = i; j < entry->shared_count - 1; j++) {
                strcpy(entry->shared_users[j], entry->shared_users[j + 1]);
            }

            entry->shared_count--;

            if (entry->shared_count == 0) {
                entry->shared = false;
            }

            return;
        }
    }
}


/* Check if canvas is in use by other clients (excluding exclude_client) */
bool is_canvas_in_use_by_others(handle_t canvas_handle, struct client_info *exclude_client) {
    if (canvas_handle == 0 || canvas_handle >= next_handle) return false;

    struct handle_entry *entry = &handle_table[canvas_handle];

    pthread_mutex_lock(&client_list_mutex);

    for (int i = 0; i < client_count; i++) {
        struct client_info *c = all_clients[i];

        if (c == exclude_client) continue;

        for (int j = 0; j < c->owned_count; j++) {
            if (c->owned_canvases[j] == canvas_handle) {
                pthread_mutex_unlock(&client_list_mutex);
                return true;
            }
        }
    }

    pthread_mutex_unlock(&client_list_mutex);

    if (entry->shared && entry->shared_count > 0) return true;

    return false;
}


/* Remove handle from table */
void remove_handle(handle_t handle) {
    if (handle == 0 || handle >= next_handle) return;

    handle_table[handle].ptr = NULL;
    handle_table[handle].type = HANDLE_NONE;
    handle_table[handle].shared = false;
    handle_table[handle].shared_count = 0;
}


/* Add canvas to client's owned list */
void add_owned_canvas(struct client_info *client, handle_t handle) {
    if (client->owned_count >= MAX_OWNED_CANVASES) return;

    client->owned_canvases[client->owned_count] = handle;
    client->owned_count++;
}


/* Remove canvas from client's owned list */
void remove_owned_canvas(struct client_info *client, handle_t handle) {
    for (int i = 0; i < client->owned_count; i++) {
        if (client->owned_canvases[i] == handle) {
            for (int j = i; j < client->owned_count - 1; j++) {
                client->owned_canvases[j] = client->owned_canvases[j + 1];
            }

            client->owned_count--;
            break;
        }
    }
}


/* Send reply to client via FIFO */
void send_reply(int fd_write, const char *message) {
    write(fd_write, message, strlen(message));
}


/* Check user credentials */
int check_user(const char *username, int *balance) {
    FILE *file = fopen("users.txt", "r");

    if (file == NULL) return -1;

    char line[256];

    while (fgets(line, sizeof(line), file) != NULL) {
        char file_username[64];
        int file_balance;

        if (sscanf(line, "%63s %d", file_username, &file_balance) == 2) {
            if (strcmp(username, file_username) == 0) {
                *balance = file_balance;
                fclose(file);
                return 1;
            }
        }
    }

    fclose(file);
    return 0;
}


/* Client list management */
void add_client_to_list(struct client_info *client) {
    pthread_mutex_lock(&client_list_mutex);

    if (client_count < MAX_CLIENTS) {
        all_clients[client_count++] = client;
    }

    pthread_mutex_unlock(&client_list_mutex);
}


void remove_client_from_list(struct client_info *client) {
    pthread_mutex_lock(&client_list_mutex);

    for (int i = 0; i < client_count; i++) {
        if (all_clients[i] == client) {
            for (int j = i; j < client_count - 1; j++) {
                all_clients[j] = all_clients[j + 1];
            }

            client_count--;
            break;
        }
    }

    pthread_mutex_unlock(&client_list_mutex);
}


void check_client_alive(struct client_info *client) {
    if (client == NULL) return;

    if (kill(client->pid, 0) == -1 && errno == ESRCH) {
        client->should_disconnect = true;
    }
}


/* Generate animation using ffmpeg */
int generate_animation(struct canvas *canvas, const char *filename,
                       int start_frame, int end_frame, int frame_rate, int fd_write) {
    if (canvas == NULL || filename == NULL) {
        send_reply(fd_write, "-2\n");
        return -1;
    }

    if (start_frame < 0 || end_frame < start_frame || frame_rate <= 0) {
        send_reply(fd_write, "-2\n");
        return -1;
    }

    size_t frame_size = animate_frame_size_bytes(canvas);

    if (frame_size == 0) {
        send_reply(fd_write, "-3\n");
        return -1;
    }

    size_t width = animate_get_width(canvas);
    size_t height = animate_get_height(canvas);

    if (width == 0 || height == 0) {
        send_reply(fd_write, "-3\n");
        return -1;
    }

    char dat_filename[512], mp4_filename[512], log_filename[512];
    snprintf(dat_filename, sizeof(dat_filename), "%s.dat", filename);
    snprintf(mp4_filename, sizeof(mp4_filename), "%s.mp4", filename);
    snprintf(log_filename, sizeof(log_filename), "%s.log", filename);

    FILE *dat_file = fopen(dat_filename, "wb");

    if (dat_file == NULL) {
        send_reply(fd_write, "0 -1\n");
        return -1;
    }

    void *frame_buffer = malloc(frame_size);

    if (frame_buffer == NULL) {
        fclose(dat_file);
        send_reply(fd_write, "-3\n");
        return -1;
    }

    for (int frame = start_frame; frame <= end_frame; frame++) {
        animate_generate_frame(canvas, frame, frame_rate, frame_buffer);

        if (fwrite(frame_buffer, 1, frame_size, dat_file) != frame_size) {
            free(frame_buffer);
            fclose(dat_file);
            send_reply(fd_write, "0 -1\n");
            return -1;
        }
    }

    free(frame_buffer);
    fclose(dat_file);

    pid_t pid = fork();

    if (pid == -1) {
        send_reply(fd_write, "-3\n");
        return -1;
    }

    if (pid == 0) {
        freopen(log_filename, "w", stderr);
        freopen(log_filename, "a", stdout);

        char frame_rate_str[32], width_str[32], height_str[32];
        snprintf(frame_rate_str, sizeof(frame_rate_str), "%d", frame_rate);
        snprintf(width_str, sizeof(width_str), "%zu", width);
        snprintf(height_str, sizeof(height_str), "%zu", height);

        execlp("ffmpeg", "ffmpeg",
               "-f", "rawvideo",
               "-pix_fmt", "rgba",
               "-s", width_str, "x", height_str,
               "-r", frame_rate_str,
               "-i", dat_filename,
               "-c:v", "libx264",
               "-y", mp4_filename,
               NULL);

        exit(1);
    }

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        send_reply(fd_write, "0\n");
    } else {
        send_reply(fd_write, "0 0 -1\n");
    }

    return 0;
}


/* Queue functions */
void init_queue() {
    request_queue.front = 0;
    request_queue.rear = 0;
    request_queue.count = 0;
    pthread_mutex_init(&request_queue.mutex, NULL);
    pthread_cond_init(&request_queue.not_empty, NULL);
    pthread_cond_init(&request_queue.not_full, NULL);
}


void push_request(struct request req) {
    pthread_mutex_lock(&request_queue.mutex);

    while (request_queue.count >= MAX_QUEUE_SIZE) {
        pthread_cond_wait(&request_queue.not_full, &request_queue.mutex);
    }

    request_queue.items[request_queue.rear] = req;
    request_queue.rear = (request_queue.rear + 1) % MAX_QUEUE_SIZE;
    request_queue.count++;
    pthread_cond_signal(&request_queue.not_empty);
    pthread_mutex_unlock(&request_queue.mutex);
}


struct request pop_request() {
    pthread_mutex_lock(&request_queue.mutex);

    while (request_queue.count == 0) {
        pthread_cond_wait(&request_queue.not_empty, &request_queue.mutex);
    }

    struct request req = request_queue.items[request_queue.front];
    request_queue.front = (request_queue.front + 1) % MAX_QUEUE_SIZE;
    request_queue.count--;
    pthread_cond_signal(&request_queue.not_full);
    pthread_mutex_unlock(&request_queue.mutex);

    return req;
}


/* SIGUSR1 handler */
void handle_sigusr1(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)context;
    client_pid = info->si_pid;
}


/* Client handler thread */
void *client_handler(void *arg) {
    struct client_info *client = (struct client_info *)arg;

    add_client_to_list(client);

    int fd_read = open(client->fifo_c2s, O_RDONLY);

    if (fd_read == -1) {
        perror("open c2s");
        remove_client_from_list(client);
        free(client);
        return NULL;
    }

    int fd_write = open(client->fifo_s2c, O_WRONLY);

    if (fd_write == -1) {
        perror("open s2c");
        close(fd_read);
        remove_client_from_list(client);
        free(client);
        return NULL;
    }

    printf("Client connected: %d\n", client->pid);

    char buffer[BUFFER_SIZE];

    while (1) {
        check_client_alive(client);

        if (client->should_disconnect) break;

        fd_set readfds;
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        FD_ZERO(&readfds);
        FD_SET(fd_read, &readfds);

        int select_result = select(fd_read + 1, &readfds, NULL, NULL, &tv);

        if (select_result == -1) break;
        if (select_result == 0) continue;

        ssize_t bytes_read = read(fd_read, buffer, BUFFER_SIZE - 1);

        if (bytes_read <= 0) break;

        buffer[bytes_read] = '\0';

        struct request req;
        req.client = client;
        req.fd_write = fd_write;
        strcpy(req.command, buffer);
        push_request(req);
    }

    /* Cleanup resources owned only by this client (preserve shared canvases) */
    for (handle_t i = 1; i < next_handle; i++) {
        if (handle_table[i].owner == client) {
            if (handle_table[i].type == HANDLE_CANVAS) {
                if (!is_canvas_in_use_by_others(i, client)) {
                    struct canvas *canvas = handle_table[i].ptr;
                    if (canvas) animate_destroy_canvas(canvas);
                    remove_handle(i);
                } else {
                    handle_table[i].owner = NULL;
                    remove_owned_canvas(client, i);
                }
            } else if (handle_table[i].type == HANDLE_PLACEMENT) {
                struct placement_wrapper *pw = handle_table[i].ptr;

                if (pw) {
                    if (pw->placement_ptr) animate_destroy_placement(pw->placement_ptr);
                    free(pw);
                }

                remove_handle(i);
            } else if (handle_table[i].type == HANDLE_SPRITE) {
                struct sprite *sp = handle_table[i].ptr;
                if (sp) animate_destroy_sprite(sp);
                remove_handle(i);
            }
        } else if (handle_table[i].shared && handle_table[i].owner == NULL) {
            for (int j = 0; j < handle_table[i].shared_count; j++) {
                if (strcmp(handle_table[i].shared_users[j], client->username) == 0) {
                    remove_shared_user(i, client->username);
                    break;
                }
            }
        }
    }

    close(fd_read);
    close(fd_write);
    unlink(client->fifo_c2s);
    unlink(client->fifo_s2c);

    printf("Client disconnected: %d\n", client->pid);

    remove_client_from_list(client);
    free(client);

    return NULL;
}


/* Process request */
void process_request(struct request req) {
    struct client_info *client = req.client;
    int fd_write = req.fd_write;
    char *cmd = req.command;

    if (strncmp(cmd, "Login ", 6) == 0) {
        char username[64];
        sscanf(cmd + 6, "%63s", username);

        int balance;
        int result = check_user(username, &balance);

        if (result == 1 && balance > 0) {
            client->logged_in = true;
            strcpy(client->username, username);

            char reply[BUFFER_SIZE];
            snprintf(reply, BUFFER_SIZE, "%d\n", balance);
            send_reply(fd_write, reply);
        } else if (result == 1 && balance <= 0) {
            send_reply(fd_write, "Reject BALANCE\n");
            client->should_disconnect = true;
        } else {
            send_reply(fd_write, "Reject UNAUTHORISED\n");
            client->should_disconnect = true;
        }

        return;
    }

    if (strncmp(cmd, "create_canvas ", 14) == 0) {
        size_t height, width;

        if (sscanf(cmd + 14, "%zu %zu", &height, &width) != 2) {
            send_reply(fd_write, "-1\n");
            return;
        }

        color_t background = animate_color_argb(255, 255, 255, 255);
        struct canvas *canvas = animate_create_canvas(height, width, background);

        if (!canvas) {
            send_reply(fd_write, "-2\n");
            return;
        }

        handle_t handle = create_handle(canvas, HANDLE_CANVAS, client);

        if (handle == 0) {
            animate_destroy_canvas(canvas);
            send_reply(fd_write, "-2\n");
            return;
        }

        add_owned_canvas(client, handle);

        char reply[BUFFER_SIZE];
        snprintf(reply, BUFFER_SIZE, "0 %lu\n", handle);
        send_reply(fd_write, reply);

        return;
    }

    if (strncmp(cmd, "create_rectangle ", 17) == 0) {
        int r, g, b;
        size_t w, h;

        if (sscanf(cmd + 17, "%d %d %d %zu %zu", &r, &g, &b, &w, &h) != 5) {
            send_reply(fd_write, "-1\n");
            return;
        }

        color_t color = animate_color_argb(255, r, g, b);
        struct sprite *sprite = animate_create_rectangle(w, h, color, true);

        if (!sprite) {
            send_reply(fd_write, "-2\n");
            return;
        }

        handle_t handle = create_handle(sprite, HANDLE_SPRITE, client);

        if (handle == 0) {
            animate_destroy_sprite(sprite);
            send_reply(fd_write, "-2\n");
            return;
        }

        char reply[BUFFER_SIZE];
        snprintf(reply, BUFFER_SIZE, "0 %lu\n", handle);
        send_reply(fd_write, reply);

        return;
    }

    if (strncmp(cmd, "create_circle ", 14) == 0) {
        int r, g, b;
        size_t rad;

        if (sscanf(cmd + 14, "%d %d %d %zu", &r, &g, &b, &rad) != 4) {
            send_reply(fd_write, "-1\n");
            return;
        }

        color_t color = animate_color_argb(255, r, g, b);
        struct sprite *sprite = animate_create_circle(rad, color, true);

        if (!sprite) {
            send_reply(fd_write, "-2\n");
            return;
        }

        handle_t handle = create_handle(sprite, HANDLE_SPRITE, client);

        if (handle == 0) {
            animate_destroy_sprite(sprite);
            send_reply(fd_write, "-2\n");
            return;
        }

        char reply[BUFFER_SIZE];
        snprintf(reply, BUFFER_SIZE, "0 %lu\n", handle);
        send_reply(fd_write, reply);

        return;
    }

    if (strncmp(cmd, "add_to_canvas ", 14) == 0) {
        handle_t ch, sh;
        int x, y;

        if (sscanf(cmd + 14, "%lu %lu %d %d", &ch, &sh, &x, &y) != 4) {
            send_reply(fd_write, "-1\n");
            return;
        }

        struct canvas *canvas = get_pointer(ch, HANDLE_CANVAS);
        struct sprite *sprite = get_pointer(sh, HANDLE_SPRITE);

        if (!canvas || !sprite) {
            send_reply(fd_write, "-2\n");
            return;
        }

        struct sprite_placement *pp = animate_place_sprite(canvas, sprite, x, y);

        if (!pp) {
            send_reply(fd_write, "-2\n");
            return;
        }

        handle_t ph = create_placement_wrapper_handle(canvas, sprite, x, y, client, pp);

        if (ph == 0) {
            animate_destroy_placement(pp);
            send_reply(fd_write, "-2\n");
            return;
        }

        char reply[BUFFER_SIZE];
        snprintf(reply, BUFFER_SIZE, "0 %lu\n", ph);
        send_reply(fd_write, reply);

        return;
    }

    if (strncmp(cmd, "move ", 5) == 0) {
        handle_t ph;
        int nx, ny;

        if (sscanf(cmd + 5, "%lu %d %d", &ph, &nx, &ny) != 3) {
            send_reply(fd_write, "-1\n");
            return;
        }

        if (!client_owns_handle(client, ph)) {
            send_reply(fd_write, "-2\n");
            return;
        }

        struct placement_wrapper *pw = get_pointer(ph, HANDLE_PLACEMENT);

        if (!pw) {
            send_reply(fd_write, "-2\n");
            return;
        }

        pw->x = nx;
        pw->y = ny;
        send_reply(fd_write, "0\n");

        return;
    }

    if (strncmp(cmd, "remove_sprite ", 14) == 0) {
        handle_t ph;

        if (sscanf(cmd + 14, "%lu", &ph) != 1) {
            send_reply(fd_write, "-1\n");
            return;
        }

        if (!client_owns_handle(client, ph)) {
            send_reply(fd_write, "-2\n");
            return;
        }

        struct placement_wrapper *pw = get_pointer(ph, HANDLE_PLACEMENT);

        if (!pw) {
            send_reply(fd_write, "-2\n");
            return;
        }

        if (pw->placement_ptr) {
            animate_destroy_placement(pw->placement_ptr);
        }

        free(pw);
        remove_handle(ph);
        send_reply(fd_write, "0\n");

        return;
    }

    if (strncmp(cmd, "generate ", 9) == 0) {
        handle_t ch;
        char fname[256];
        int sf, ef, fr;

        if (sscanf(cmd + 9, "%lu %255s %d %d %d", &ch, fname, &sf, &ef, &fr) != 5) {
            send_reply(fd_write, "-1\n");
            return;
        }

        struct canvas *canvas = get_pointer(ch, HANDLE_CANVAS);

        if (!canvas) {
            send_reply(fd_write, "-2\n");
            return;
        }

        generate_animation(canvas, fname, sf, ef, fr, fd_write);

        return;
    }

    if (strncmp(cmd, "share_canvas ", 13) == 0) {
        handle_t ch;
        char tuser[64];

        if (sscanf(cmd + 13, "%lu %63s", &ch, tuser) != 2) {
            send_reply(fd_write, "-1\n");
            return;
        }

        if (handle_table[ch].owner != client) {
            send_reply(fd_write, "-2\n");
            return;
        }

        if (handle_table[ch].type != HANDLE_CANVAS) {
            send_reply(fd_write, "-2\n");
            return;
        }

        int bal;

        if (check_user(tuser, &bal) != 1) {
            send_reply(fd_write, "-2\n");
            return;
        }

        add_shared_user(ch, tuser);
        send_reply(fd_write, "0\n");

        return;
    }

    if (strncmp(cmd, "barrier ", 8) == 0) {
        handle_t ch;

        if (sscanf(cmd + 8, "%lu", &ch) != 1) {
            send_reply(fd_write, "-1\n");
            return;
        }

        send_reply(fd_write, "0\n");

        return;
    }

    if (strncmp(cmd, "destroy_canvas ", 15) == 0) {
        handle_t ch;

        if (sscanf(cmd + 15, "%lu", &ch) != 1) {
            send_reply(fd_write, "-1\n");
            return;
        }

        struct canvas *canvas = get_pointer(ch, HANDLE_CANVAS);

        if (!canvas) {
            send_reply(fd_write, "-2\n");
            return;
        }

        if (handle_table[ch].owner != client) {
            send_reply(fd_write, "-2\n");
            return;
        }

        animate_destroy_canvas(canvas);
        remove_handle(ch);
        remove_owned_canvas(client, ch);
        send_reply(fd_write, "0\n");

        return;
    }

    if (strcmp(cmd, "disconnect\n") == 0) {
        send_reply(fd_write, "BYE\n");
        client->should_disconnect = true;
        return;
    }

    send_reply(fd_write, "Unknown command\n");
}


/* Worker thread */
void *worker_thread(void *arg) {
    (void)arg;

    while (1) {
        struct request req = pop_request();
        process_request(req);
    }

    return NULL;
}


/* Main */
int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <threadpool size>\n", argv[0]);
        return 1;
    }

    init_queue();

    printf("Server PID: %d\n", getpid());
    fflush(stdout);

    struct sigaction sa;
    sa.sa_sigaction = handle_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;

    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction");
        return 1;
    }

    int threadpool_size = atoi(argv[1]);
    pthread_t *workers = malloc(threadpool_size * sizeof(pthread_t));

    if (!workers) {
        perror("malloc");
        return 1;
    }
is_canvas_in_use_by_others
    for (int i = 0; i < threadpool_size; i++) {
        pthread_create(&workers[i], NULL, worker_thread, NULL);
    }

    while (1) {
        if (client_pid != 0) {
            pid_t pid = client_pid;
            client_pid = 0;

            struct client_info *client = calloc(1, sizeof(struct client_info));

            if (!client) {
                perror("calloc");
                continue;
            }

            client->pid = pid;

            snprintf(client->fifo_c2s, FIFO_NAME_SIZE, "FIFO_C2S_%d", pid);
            snprintf(client->fifo_s2c, FIFO_NAME_SIZE, "FIFO_S2C_%d", pid);

            unlink(client->fifo_c2s);
            unlink(client->fifo_s2c);

            if (mkfifo(client->fifo_c2s, 0666) == -1) {
                perror("mkfifo c2s");
                free(client);
                continue;
            }

            if (mkfifo(client->fifo_s2c, 0666) == -1) {
                perror("mkfifo s2c");
                unlink(client->fifo_c2s);
                free(client);
                continue;
            }

            kill(pid, SIGUSR2);

            pthread_t thread;

            if (pthread_create(&thread, NULL, client_handler, client) != 0) {
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

    free(workers);
    return 0;
}