/* js_dispatch.h */
#ifndef JS_DISPATCH_H
#define JS_DISPATCH_H

typedef void (*js_dispatch_fn)(void *arg);

/* Any module calls this to safely run fn(arg) on the thread that owns
   the app's JSContext — regardless of what that thread's main loop is. */
void js_dispatch_to_main(js_dispatch_fn fn, void *arg);

/* The module that OWNS the main loop (webview.c) calls this once, at
   startup, to say "here's how dispatch actually works for this app." */
typedef void (*js_dispatch_impl_fn)(js_dispatch_fn fn, void *arg);
void js_dispatch_set_impl(js_dispatch_impl_fn impl);

#endif
