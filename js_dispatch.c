/* js_dispatch.c */
#include "js_dispatch.h"
#include <stddef.h>
#include <stdio.h>

static js_dispatch_impl_fn g_impl = NULL;

void js_dispatch_set_impl(js_dispatch_impl_fn impl) {
    g_impl = impl;
}

void js_dispatch_to_main(js_dispatch_fn fn, void *arg) {
    if (g_impl) {
        g_impl(fn, arg);
    } else {
        /* No GUI main loop registered — headless p2pClient case.
           Fill this in with your existing pipe + os.setReadHandler
           mechanism if you need libdatachannel.c to work standalone
           (without webview) too. For now, fail loudly rather than
           silently dropping events. */
        fprintf(stderr, "js_dispatch_to_main: no dispatch impl registered\n");
    }
}
