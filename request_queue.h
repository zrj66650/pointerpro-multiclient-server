#ifndef REQUEST_QUEUE_H
#define REQUEST_QUEUE_H

#include "common.h"

#define MAX_QUEUE_SIZE 1024

struct request {

    struct client_info *client;

    int fd_write;

    char command[BUFFER_SIZE];
};

struct request_queue {

    struct request items[MAX_QUEUE_SIZE];

    int front;

    int rear;

    int count;

    pthread_mutex_t mutex;

    pthread_cond_t not_empty;

    pthread_cond_t not_full;
};

#endif