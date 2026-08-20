/* js_dispatch.h */
#ifndef JS_DISPATCH_H
#define JS_DISPATCH_H

#include <stdint.h>

typedef void (*js_dispatch_fn)(void *arg);

/* Any module calls this to safely run fn(arg) on the thread that owns
   the app's JSContext — regardless of what that thread's main loop is. */
void js_dispatch_to_main(js_dispatch_fn fn, void *arg);

/* The module that OWNS the main loop (webview.c) calls this once, at
   startup, to say "here's how dispatch actually works for this app." */
typedef void (*js_dispatch_impl_fn)(js_dispatch_fn fn, void *arg);
void js_dispatch_set_impl(js_dispatch_impl_fn impl);

/* Headless (no GUI main loop) fallback: wakes js_std_loop via a pipe.
   wake_write_fd is the write end of a pipe whose read end the JS side
   registers with os.setReadHandler. */
void js_dispatch_init_pipe_fallback(int wake_write_fd);

/* Called from the JS side's os.setReadHandler callback on the pipe's
   read end, after draining the wake byte(s). Runs all queued callbacks. */
void js_dispatch_drain_pipe_queue(void);

#endif