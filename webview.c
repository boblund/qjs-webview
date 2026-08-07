#include "quickjs.h"
#include "webview.h"
#include <string.h>
#include <stdlib.h>

static JSClassID js_webview_class_id;

typedef struct bind_ctx_s bind_ctx_t;
struct bind_ctx_s {
    JSContext  *ctx;
    JSValue     func;
    char       *name;
    webview_t   w;      /* <-- add this, set to data->w in js_webview_bind */
    bind_ctx_t *next;
};

typedef struct {
    webview_t   w;
    bind_ctx_t *binds;  /* head of linked list of active binds, for cleanup */
} webview_data_t;

/* ---- handle wrapper ---- */

static void js_webview_finalizer(JSRuntime *rt, JSValue val) {
    webview_data_t *data = JS_GetOpaque(val, js_webview_class_id);
    if (!data) return;

    bind_ctx_t *bc = data->binds;
    while (bc) {
        bind_ctx_t *next = bc->next;
        JS_FreeValueRT(rt, bc->func);   /* RT variant — no live JSContext during finalization */
        free(bc->name);
        free(bc);
        bc = next;
    }

    webview_destroy(data->w);
    free(data);
}

static JSClassDef js_webview_class = {
    "Webview",
    .finalizer = js_webview_finalizer,
};

static JSValue js_webview_ctor(JSContext *ctx, JSValueConst new_target,
                                int argc, JSValueConst *argv) {
    int debug = 0;
    if (argc > 0) JS_ToInt32(ctx, &debug, argv[0]);

    webview_t w = webview_create(debug, NULL);
    if (!w) return JS_ThrowInternalError(ctx, "webview_create failed");

    webview_data_t *data = malloc(sizeof(webview_data_t));
    data->w = w;
    data->binds = NULL;

    JSValue proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) { webview_destroy(w); free(data); return proto; }

    JSValue obj = JS_NewObjectProtoClass(ctx, proto, js_webview_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) { webview_destroy(w); free(data); return obj; }

    JS_SetOpaque(obj, data);
    return obj;
}

static webview_data_t *get_data(JSContext *ctx, JSValueConst this_val) {
    webview_data_t *data = JS_GetOpaque2(ctx, this_val, js_webview_class_id);
    if (!data) {
        JS_ThrowTypeError(ctx, "Webview handle already destroyed");
        return NULL;
    }
    return data;
}

static JSValue js_webview_destroy(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    webview_data_t *data = get_data(ctx, this_val);
    if (!data) return JS_EXCEPTION;

    bind_ctx_t *bc = data->binds;
    while (bc) {
        bind_ctx_t *next = bc->next;
        JS_FreeValue(ctx, bc->func);
        free(bc->name);
        free(bc);
        bc = next;
    }

    webview_destroy(data->w);
    free(data);
    JS_SetOpaque(this_val, NULL);
    return JS_UNDEFINED;
}

/* ---- simple pass-throughs ---- */

static JSValue js_webview_run(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
		webview_data_t *data = get_data(ctx, this_val);
		if (!data) return JS_EXCEPTION;
		webview_t w = data->w;
    webview_run(w);
    return JS_UNDEFINED;
}

static JSValue js_webview_terminate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
		webview_data_t *data = get_data(ctx, this_val);
		if (!data) return JS_EXCEPTION;
		webview_t w = data->w;
    webview_terminate(w);
    return JS_UNDEFINED;
}

static JSValue js_webview_set_title(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
		webview_data_t *data = get_data(ctx, this_val);
		if (!data) return JS_EXCEPTION;
		webview_t w = data->w;
    const char *title = JS_ToCString(ctx, argv[0]);
    webview_set_title(w, title);
    JS_FreeCString(ctx, title);
    return JS_UNDEFINED;
}

static JSValue js_webview_set_size(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
		webview_data_t *data = get_data(ctx, this_val);
		if (!data) return JS_EXCEPTION;
		webview_t w = data->w;
    int width, height, hint = WEBVIEW_HINT_NONE;
    JS_ToInt32(ctx, &width, argv[0]);
    JS_ToInt32(ctx, &height, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &hint, argv[2]);
    webview_set_size(w, width, height, hint);
    return JS_UNDEFINED;
}

static JSValue js_webview_navigate(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
		webview_data_t *data = get_data(ctx, this_val);
		if (!data) return JS_EXCEPTION;
		webview_t w = data->w;
    const char *url = JS_ToCString(ctx, argv[0]);
    webview_navigate(w, url);
    JS_FreeCString(ctx, url);
    return JS_UNDEFINED;
}

static JSValue js_webview_set_html(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
		webview_data_t *data = get_data(ctx, this_val);
		if (!data) return JS_EXCEPTION;
		webview_t w = data->w;
    const char *html = JS_ToCString(ctx, argv[0]);
    webview_set_html(w, html);
    JS_FreeCString(ctx, html);
    return JS_UNDEFINED;
}

static JSValue js_webview_eval(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
		webview_data_t *data = get_data(ctx, this_val);
		if (!data) return JS_EXCEPTION;
		webview_t w = data->w;
    const char *js = JS_ToCString(ctx, argv[0]);
    webview_eval(w, js);
    JS_FreeCString(ctx, js);
    return JS_UNDEFINED;
}

/* ---- bind/unbind: the tricky part ---- */

static void bind_callback(const char *seq, const char *req, void *arg) {
    bind_ctx_t *bc = (bind_ctx_t *)arg;
    JSContext *ctx = bc->ctx;
    webview_t w = bc->w;
		if (!w) return;  /* handle already destroyed, nothing to return a result to */

    JSValue args_val = JS_ParseJSON(ctx, req, strlen(req), "<bind>");
    int argc = 0;
    JSValue *argv = NULL;
    if (JS_IsArray(ctx, args_val)) {
        JSValue lenv = JS_GetPropertyStr(ctx, args_val, "length");
        JS_ToInt32(ctx, &argc, lenv);
        JS_FreeValue(ctx, lenv);
        argv = malloc(sizeof(JSValue) * argc);
        for (int i = 0; i < argc; i++)
            argv[i] = JS_GetPropertyUint32(ctx, args_val, i);
    }

    JSValue ret = JS_Call(ctx, bc->func, JS_UNDEFINED, argc, (JSValueConst *)argv);

    int status = 0;
    JSValue result_val = ret;
    if (JS_IsException(ret)) {
        status = 1;
        result_val = JS_GetException(ctx);
    }

    JSValue json = JS_JSONStringify(ctx, result_val, JS_UNDEFINED, JS_UNDEFINED);
    const char *result_cstr = JS_IsUndefined(json) ? "null" : JS_ToCString(ctx, json);

    webview_return(w, seq, status, result_cstr);

    if (!JS_IsUndefined(json)) JS_FreeCString(ctx, result_cstr);
    JS_FreeValue(ctx, json);
    JS_FreeValue(ctx, result_val);
    JS_FreeValue(ctx, args_val);
    for (int i = 0; i < argc; i++) JS_FreeValue(ctx, argv[i]);
    free(argv);

    /* drain microtasks queued by the callback (promise resolutions etc.) */
    JSContext *ctx1;
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &ctx1) > 0);
}

static JSValue js_webview_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    webview_data_t *data = get_data(ctx, this_val);
    if (!data) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[0]);

    bind_ctx_t *bc = malloc(sizeof(bind_ctx_t));
    bc->ctx  = ctx;
    bc->func = JS_DupValue(ctx, argv[1]);
    bc->name = strdup(name);
    bc->next = data->binds;
		bc->w = data->w;
    data->binds = bc;

    webview_bind(data->w, name, bind_callback, bc);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

static JSValue js_webview_unbind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    webview_data_t *data = get_data(ctx, this_val);
    if (!data) return JS_EXCEPTION;
    const char *name = JS_ToCString(ctx, argv[0]);

    bind_ctx_t **pp = &data->binds;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            bind_ctx_t *dead = *pp;
            *pp = dead->next;
            JS_FreeValue(ctx, dead->func);
            free(dead->name);
            free(dead);
            break;
        }
        pp = &(*pp)->next;
    }

    webview_unbind(data->w, name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

/* ---- module registration ---- */

static const JSCFunctionListEntry proto_funcs[] = {
    JS_CFUNC_DEF("run", 0, js_webview_run),
    JS_CFUNC_DEF("terminate", 0, js_webview_terminate),
    JS_CFUNC_DEF("destroy", 0, js_webview_destroy),
    JS_CFUNC_DEF("setTitle", 1, js_webview_set_title),
    JS_CFUNC_DEF("setSize", 3, js_webview_set_size),
    JS_CFUNC_DEF("navigate", 1, js_webview_navigate),
    JS_CFUNC_DEF("setHtml", 1, js_webview_set_html),
    JS_CFUNC_DEF("eval", 1, js_webview_eval),
    JS_CFUNC_DEF("bind", 2, js_webview_bind),
    JS_CFUNC_DEF("unbind", 1, js_webview_unbind),
};

static int js_webview_init(JSContext *ctx, JSModuleDef *m) {
    JS_NewClassID(&js_webview_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_webview_class_id, &js_webview_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, proto_funcs, sizeof(proto_funcs)/sizeof(proto_funcs[0]));
    JS_SetClassProto(ctx, js_webview_class_id, proto);

    JSValue webview_class = JS_NewCFunction2(ctx, js_webview_ctor, "Webview", 1,
                                              JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, webview_class, proto);
    JS_SetModuleExport(ctx, m, "Webview", webview_class);
    return 0;
}

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_webview   /* must match the cname after the comma in -M */
#endif

JSModuleDef *JS_INIT_MODULE(JSContext *ctx, const char *module_name) {
    JSModuleDef *m = JS_NewCModule(ctx, module_name, js_webview_init);
    if (!m) return NULL;
    JS_AddModuleExport(ctx, m, "Webview");
    return m;
}
