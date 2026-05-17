#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>

#include <stdint.h>
#include <stdbool.h>

#include <unistd.h>
#include <signal.h>

#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>

#include <string.h>
#include <errno.h>

#include <pthread.h>

#define FIFO_NAME_SIZE 128

#define BUFFER_SIZE 1024

#define MAX_HANDLES 2048

#define MAX_CLIENTS 64

#define MAX_OWNED_CANVASES 128

#define MAX_SHARED_USERS 32

typedef uint64_t handle_t;

typedef enum {

    HANDLE_NONE = 0,

    HANDLE_CANVAS,

    HANDLE_SPRITE,

    HANDLE_PLACEMENT

} handle_type_t;

struct client_info;

#endif