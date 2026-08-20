/* js_dispatch.c */
#include "js_dispatch.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

typedef struct pending_dispatch {
    js_dispatch_fn fn;
    void *arg;
    struct pending_dispatch *next;
} pending_dispatch_t;

static js_dispatch_impl_fn g_impl = NULL;

static pthread_mutex_t g_pipe_lock = PTHREAD_MUTEX_INITIALIZER;
static pending_dispatch_t *g_pipe_queue = NULL;
static int g_wake_write_fd = -1;

void js_dispatch_set_impl(js_dispatch_impl_fn impl) {
    g_impl = impl;
}

void js_dispatch_init_pipe_fallback(int wake_write_fd) {
    g_wake_write_fd = wake_write_fd;
}

static void pipe_fallback_impl(js_dispatch_fn fn, void *arg) {
    pending_dispatch_t *node = malloc(sizeof(*node));
    node->fn = fn;
    node->arg = arg;

    pthread_mutex_lock(&g_pipe_lock);
    node->next = g_pipe_queue;
    g_pipe_queue = node;
    pthread_mutex_unlock(&g_pipe_lock);

    uint8_t wake_byte = 1;
    /* best-effort: if this write fails (e.g. pipe full), the queue still
       has the entry — a later successful wake will drain everything
       queued so far, so a lost wake byte here isn't a lost callback */
    write(g_wake_write_fd, &wake_byte, 1);
}

void js_dispatch_drain_pipe_queue(void) {
    pthread_mutex_lock(&g_pipe_lock);
    pending_dispatch_t *head = g_pipe_queue;
    g_pipe_queue = NULL;
    pthread_mutex_unlock(&g_pipe_lock);

    while (head) {
        pending_dispatch_t *next = head->next;
        head->fn(head->arg);
        free(head);
        head = next;
    }
}

void js_dispatch_to_main(js_dispatch_fn fn, void *arg) {
    if (g_impl) {
        g_impl(fn, arg);
    } else if (g_wake_write_fd >= 0) {
        pipe_fallback_impl(fn, arg);
    } else {
        fprintf(stderr, "js_dispatch_to_main: no dispatch impl registered\n");
    }
}
