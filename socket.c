#include "quickjs.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <netdb.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "js_dispatch.h"

/* ---- dispatch pipe-fallback init/drain, exposed to JS for headless apps ---- */

static JSValue js_dispatch_init(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int fds[2];
    if (pipe(fds) != 0) return JS_ThrowInternalError(ctx, "pipe() failed");
    js_dispatch_init_pipe_fallback(fds[1]);
    return JS_NewInt32(ctx, fds[0]);
}

static JSValue js_dispatch_drain(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    js_dispatch_drain_pipe_queue();
    return JS_UNDEFINED;
}

#define countof(x) (sizeof(x) / sizeof((x)[0]))

#ifdef JS_SHARED_LIBRARY
#define JS_INIT_MODULE js_init_module
#else
#define JS_INIT_MODULE js_init_module_socket
#endif

static pthread_t global_accept_thread;

static void sigint_handler(int sig) {
    (void)sig;
		// do any cleanup
		raise(SIGUSR1); // let JS side do any cleanup
}

typedef struct {
    SSL *ssl;
		SSL_CTX* ctx;
    int fds[2];
		void* client_s;
		bool dispatch;
} ssl_thread_arg_t;

void create_ssl_thread( SSL* ssl, SSL_CTX* ctx, void* func( void*), int* fds, void* client_data,  bool dispatch ){
  ssl_thread_arg_t* args = malloc( sizeof( ssl_thread_arg_t ) );
  args->ssl = ssl;
	args->ctx = ctx;
	args->client_s = client_data;
	args->dispatch = dispatch;

  int to_thread_fds[ 2 ];
  pipe( to_thread_fds );
  args->fds[0] = to_thread_fds[0];
  fds[1] = to_thread_fds[1];

  int from_thread_fds[ 2 ];
  pipe( from_thread_fds );
  args->fds[1] = from_thread_fds[1];
  fds[0] = from_thread_fds[0];
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t tid;
  pthread_create(&tid, &attr, func, (void*)args );
  pthread_attr_destroy(&attr);
  return;
}

/* Client */

typedef struct {
    int fds[2];
    _Atomic int server_ssl_fd;
    bool dispatch;
		pthread_mutex_t mode_lock;   /* guards dispatch flag + the decide-and-act pipe/emit choice */
    JSContext *jsctx;
    JSValue on_data;
    pthread_mutex_t on_data_lock;
} JSClientData;

typedef struct {
    JSContext *jsctx;
    JSValue    on_data;
    uint8_t   *data;
    uint32_t   len;
    bool       closed;
} client_dispatch_event_t;

static void client_dispatch_main_thread(void *arg) {
    client_dispatch_event_t *ev = (client_dispatch_event_t *)arg;
    JSContext *ctx = ev->jsctx;

    JSValue arg0;
    if (ev->closed) {
        arg0 = JS_NULL;
    } else {
        JSValue buf = JS_NewArrayBufferCopy(ctx, ev->data, ev->len);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue u8ctor = JS_GetPropertyStr(ctx, global, "Uint8Array");
        arg0 = JS_CallConstructor(ctx, u8ctor, 1, (JSValueConst *)&buf);
        JS_FreeValue(ctx, u8ctor);
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, buf);
    }

    JSValue ret = JS_Call(ctx, ev->on_data, JS_UNDEFINED, 1, (JSValueConst *)&arg0);
    if (JS_IsException(ret)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        fprintf(stderr, "socket dispatch handler threw: %s\n", msg);
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, arg0);
    JS_FreeValue(ctx, ev->on_data);

    JSContext *ctx1;
    while (JS_ExecutePendingJob(JS_GetRuntime(ctx), &ctx1) > 0);

    free(ev->data);
    free(ev);
}

static void client_emit_data(JSClientData *s, const uint8_t *data, uint32_t len, bool closed) {
    pthread_mutex_lock(&s->on_data_lock);
    if (JS_IsUndefined(s->on_data)) {
        pthread_mutex_unlock(&s->on_data_lock);
        return;
    }
    client_dispatch_event_t *ev = malloc(sizeof(*ev));
    ev->jsctx = s->jsctx;
    ev->on_data = JS_DupValue(s->jsctx, s->on_data);
    pthread_mutex_unlock(&s->on_data_lock);

    ev->closed = closed;
    ev->len = len;
    if (!closed && len > 0) {
        ev->data = malloc(len);
        memcpy(ev->data, data, len);
    } else {
        ev->data = NULL;
    }
    js_dispatch_to_main(client_dispatch_main_thread, ev);
}

static JSClassID js_client_class_id;

static void js_client_finalizer(JSRuntime *rt, JSValue val)
{
    JSClientData *s = JS_GetOpaque(val, js_client_class_id);
    if (s == NULL || s->fds[0] < 0 ){
			js_free_rt(rt, s);
			return;
		}

		pthread_mutex_lock(&s->on_data_lock);
    if (!JS_IsUndefined(s->on_data)) JS_FreeValueRT(rt, s->on_data);
    s->on_data = JS_UNDEFINED;
    pthread_mutex_unlock(&s->on_data_lock);
		pthread_mutex_destroy(&s->mode_lock);

		if( s->server_ssl_fd != -1 ){
				shutdown( s->server_ssl_fd, SHUT_RDWR );
				s->server_ssl_fd = -1;
		}

		if( s->fds[0] == s->fds[1] ){
			shutdown(s->fds[0], SHUT_WR); // socket
		} else {
			close( s->fds[1] ); // pipe
		}
		close( s->fds[0] );
		s->fds[0] = -1; s->fds[1] = -1;
		js_free_rt(rt, s);
}

static JSValue js_client_ctor(JSContext *ctx,
                             JSValueConst new_target,
                             int argc, JSValueConst *argv)
{
    JSClientData *s;
    JSValue obj = JS_UNDEFINED;
    JSValue proto;

    s = js_mallocz(ctx, sizeof(JSClientData));
    if (!s) return JS_EXCEPTION;
		s->on_data = JS_UNDEFINED;
    pthread_mutex_init(&s->on_data_lock, NULL);
		pthread_mutex_init(&s->mode_lock, NULL);

    /* using new_target to get the prototype is necessary when the
       class is extended. */

    proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto))
        goto fail;
    obj = JS_NewObjectProtoClass(ctx, proto, js_client_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj))
        goto fail;
    JS_SetOpaque(obj, s);
    return obj;
 fail:
    js_free(ctx, s);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_client_end(JSContext *ctx, JSValueConst this_val,int argc, JSValueConst *argv){
		JSClientData *s = JS_GetOpaque2(ctx, this_val, js_client_class_id);
    if (!s) return JS_EXCEPTION;
    if (s == NULL || s->fds[0] < 0 ) return JS_UNDEFINED;
		//printf( "js_client_end fds: %d %d, server_ssl_fd: %d\n", s->fds[0], s->fds[1], s->server_ssl_fd );
		if (s->fds[0] < 0) return JS_UNDEFINED;

		pthread_mutex_lock(&s->on_data_lock);
    if (!JS_IsUndefined(s->on_data)) JS_FreeValue(ctx, s->on_data);
    s->on_data = JS_UNDEFINED;
    pthread_mutex_unlock(&s->on_data_lock);
		pthread_mutex_destroy(&s->mode_lock);

		if( s->fds[0] == s->fds[1] ){
			shutdown(s->fds[0], SHUT_WR); // socket
		} else {
			if( s->server_ssl_fd != -1 ){
				shutdown( s->server_ssl_fd, SHUT_RDWR );
				s->server_ssl_fd = -1;
			}
			close( s->fds[1] ); // pipe
		}
		close( s->fds[0] );
		s->fds[0] = -1; s->fds[1] = -1;
		return JS_UNDEFINED;
}

SSL_CTX* create_client_ssl_ctx(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { ERR_print_errors_fp(stderr); exit(1); }

		SSL_CTX_set_options(ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
		SSL_CTX_set_quiet_shutdown(ctx, 1);

    // Skip certificate verification since server uses a self-signed cert.
    // In production: use SSL_VERIFY_PEER and load the CA cert instead.
		//SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); REMOVED FOR AWS

    return ctx;
}
void *client_ssl_thread(void *arg) {
		int server_closed = 0;
    ssl_thread_arg_t *args = (ssl_thread_arg_t *)arg;
    char buf[4096];
		struct pollfd fds[2];
		fds[0].fd     = SSL_get_fd( args->ssl );
		fds[0].events = POLLIN | POLLHUP | POLLERR;
		fds[1].fd     = args->fds[0];
		fds[1].events = POLLIN | POLLHUP;
		while (1) {
        int ready = poll(fds, 2, -1);
        if (ready < 0) { perror("poll"); break; }
        if (fds[0].revents & POLLIN) {
            int  n = SSL_read(args->ssl, buf, sizeof(buf) - 1);
            if (n <= 0){
							int err = SSL_get_error(args->ssl, n);
							if(err == SSL_ERROR_ZERO_RETURN) server_closed = 1;
							if(err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) ERR_print_errors_fp(stderr);
							break;
						}

						JSClientData *cd = (JSClientData *)args->client_s;
						pthread_mutex_lock(&cd->mode_lock);
						if (cd->dispatch) {
								client_emit_data(cd, (uint8_t *)buf, n, false);
						} else {
								write( args->fds[1], buf, n );
						}
						pthread_mutex_unlock(&cd->mode_lock);
        }

        // --- main thread sent a message ---
        if (fds[1].revents & ( POLLIN | POLLHUP ) ) {
            int n = read(args->fds[0], buf, sizeof(buf) - 1);
            if (n <= 0) { printf( "client_ssl_thread main thread read <= 0" ); break; }
						SSL_write( args->ssl, buf, n );
        }
    }
		int ssl_fd = SSL_get_fd(args->ssl);
		SSL_set_quiet_shutdown(args->ssl, 1);
    if(!server_closed) SSL_shutdown(args->ssl);
    SSL_free(args->ssl);
		SSL_CTX_free(args->ctx);
		ERR_clear_error();
		close(args->fds[0]);
		close( ssl_fd );
		JSClientData* ptr = (JSClientData*) args->client_s;
		ptr->server_ssl_fd = -1;
		pthread_mutex_lock(&ptr->mode_lock);
		if (ptr->dispatch) {
				client_emit_data(ptr, NULL, 0, true);
		} else {
				char zero = 0;
				write(args->fds[1], &zero, 1);
		}
		pthread_mutex_unlock(&ptr->mode_lock);
    free(args);
    return NULL;
}

typedef struct {
    int fd;
    JSClientData *client_s;
} plain_dispatch_arg_t;

void *plain_dispatch_thread(void *arg_) {
    plain_dispatch_arg_t *arg = (plain_dispatch_arg_t *)arg_;
    char buf[4096];
    while (1) {
        int n = read(arg->fd, buf, sizeof(buf));
        if (n <= 0) {
            client_emit_data(arg->client_s, NULL, 0, true);
            break;
        }
        client_emit_data(arg->client_s, (uint8_t *)buf, n, false);
    }
    free(arg);
    return NULL;
}

static JSValue js_client_connect(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    JSClientData *s = JS_GetOpaque2(ctx, this_val, js_client_class_id);
    if (!s) return JS_EXCEPTION;

    if (argc != 1 || JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT) {
        return JS_EXCEPTION;
    }

    // Extract 'host' as string
    JSValue js_host = JS_GetPropertyStr(ctx, argv[0], "host");
		const char *host_str = JS_IsUndefined(js_host) ? NULL : JS_ToCString(ctx, js_host);
		const char *c_host = host_str ? host_str : "localhost";
		JS_FreeValue(ctx, js_host);

		/*const char* c_host;
		if( JS_VALUE_GET_TAG( js_host ) == JS_TAG_UNDEFINED ){
			c_host = "localhost";
		} else {
			c_host = JS_ToCString( ctx, js_host );
			JS_FreeValue( ctx, js_host );
		}*/

    // Extract 'port' as number
		int port;
    JSValue js_port = JS_GetPropertyStr(ctx, argv[0], "port");
		if( JS_VALUE_GET_TAG( js_port ) == JS_TAG_UNDEFINED ){
			perror( "connect: { port } required" );
			JS_FreeValue(ctx, js_port);
			if (host_str) JS_FreeCString(ctx, host_str);
			return JS_UNDEFINED;
		} else {
			JS_ToInt32( ctx, &port, js_port );
			JS_FreeValue( ctx, js_port );
		}

		// TLS?
		bool tls = 0;
    JSValue js_tls = JS_GetPropertyStr(ctx, argv[0], "tls");
		if( JS_VALUE_GET_TAG( js_tls ) != JS_TAG_UNDEFINED ){
			tls = JS_ToBool( ctx, js_tls );
			JS_FreeValue( ctx, js_tls );
		}

	  struct addrinfo hints={0}, *res, *rp;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo( c_host, NULL, &hints, &res) != 0) {
        perror("getaddrinfo");
				if (host_str) JS_FreeCString(ctx, host_str);
        return JS_UNDEFINED;
    }
		int f;
		char v4[INET_ADDRSTRLEN] = {0};
		char v6[INET6_ADDRSTRLEN] = {0};

		for( rp = res, f = 0;  f != 3 && rp != NULL; rp = rp->ai_next ){
			if( rp->ai_family == AF_INET ){
				struct sockaddr_in *ipv4 = (struct sockaddr_in *)rp->ai_addr;
				inet_ntop(AF_INET, &ipv4->sin_addr, v4, INET_ADDRSTRLEN);
				f |= 1;
			} else if( rp->ai_family == AF_INET6 ) {
				struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)rp->ai_addr;
				inet_ntop(AF_INET6, &ipv6->sin6_addr, v6, INET6_ADDRSTRLEN);
				f |= 2;
			}
		}

		//JS_FreeCString(ctx, c_host);
		if (host_str) JS_FreeCString(ctx, host_str);  // only free if we allocated it
		freeaddrinfo(res);

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }
		s->fds[ 0 ] = s->fds[1] = client_fd;
    struct sockaddr_in server_addr; // struct sockaddr_in6 server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;  //server_addr.sin6_family = AF_INET6;
    server_addr.sin_port = htons(port); //server_addr.sin6_port = htons(port);
    if (inet_pton(AF_INET, v4, &server_addr.sin_addr) <= 0) { //if (inet_pton(AF_INET6, c_ip, &server_addr.sin6_addr) <= 0) {
        perror("inet_pton"); close(client_fd); exit(EXIT_FAILURE);
    }

    if (connect(client_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect"); close(client_fd); exit(EXIT_FAILURE);
    }

		if( tls ){
			  SSL_CTX* ssl_ctx = create_client_ssl_ctx();
				SSL *ssl = SSL_new(ssl_ctx);
				SSL_set_fd(ssl, client_fd);
				SSL_set_tlsext_host_name(ssl, c_host);  // added for AWS - sets SNI
				if (SSL_connect(ssl) <= 0) {
					fprintf(stderr, "SSL_accept failed\n");
					SSL_shutdown(ssl);
					SSL_free(ssl);
					SSL_CTX_free(ssl_ctx);
					close(client_fd);
					return JS_UNDEFINED;
				}else{
					s->server_ssl_fd = SSL_get_fd(ssl);
					create_ssl_thread( ssl, ssl_ctx, client_ssl_thread, s->fds, (void*) s, false );
				}
		}

		JSValue arr = JS_NewArray(ctx);
		JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, s->fds[0]));
		JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, s->fds[1]));
    return arr;
}

static JSValue js_client_start_dispatch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSClientData *s = JS_GetOpaque2(ctx, this_val, js_client_class_id);
    if (!s) return JS_EXCEPTION;

    if (s->fds[0] == s->fds[1]) {
        /* plain TCP: no thread has ever read this fd — spawn now, safe handoff */
        s->dispatch = true;
        plain_dispatch_arg_t *pd_arg = malloc(sizeof(*pd_arg));
        pd_arg->fd = s->fds[0];
        pd_arg->client_s = s;
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, plain_dispatch_thread, pd_arg);
        pthread_attr_destroy(&attr);
    } else {
        /* TLS: client_ssl_thread is already running in pipe mode — drain
           anything it already wrote, then flip the flag, all under the
           lock the thread itself checks before its next decision */
        pthread_mutex_lock(&s->mode_lock);

        uint8_t drain_buf[4096];
        struct pollfd pfd = { .fd = s->fds[0], .events = POLLIN };
        while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            int n = read(s->fds[0], drain_buf, sizeof(drain_buf));
            if (n <= 0) break;
            client_emit_data(s, drain_buf, n, false);
        }

        s->dispatch = true;
        pthread_mutex_unlock(&s->mode_lock);
    }
    return JS_UNDEFINED;
}


static JSValue js_client_set_data_handler(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSClientData *s = JS_GetOpaque2(ctx, this_val, js_client_class_id);
    if (!s) return JS_EXCEPTION;
    pthread_mutex_lock(&s->on_data_lock);
    if (!JS_IsUndefined(s->on_data)) JS_FreeValue(ctx, s->on_data);
    s->jsctx = ctx;
    s->on_data = JS_DupValue(ctx, argv[0]);
    pthread_mutex_unlock(&s->on_data_lock);
    return JS_UNDEFINED;
}

static JSClassDef js_client_class = {
    "Client",
    .finalizer = js_client_finalizer,
};

static const JSCFunctionListEntry js_client_proto_funcs[] = {
    JS_CFUNC_DEF("connect", 2, js_client_connect),
		JS_CFUNC_DEF("end", 0, js_client_end),
		JS_CFUNC_DEF("setDataHandler", 1, js_client_set_data_handler),
		JS_CFUNC_DEF("startDispatch", 0, js_client_start_dispatch)
};

/* Server */

#define MAX_CLIENTS 10

typedef struct {
		int socket_fd;
} JSServerData;

static JSClassID js_server_class_id;

static void js_server_finalizer(JSRuntime *rt, JSValue val)
{
    JSServerData *s = JS_GetOpaque(val, js_server_class_id);
    if (s == NULL ) return;
		if (s->socket_fd >= 0) {
			close(s->socket_fd);
			s->socket_fd = -1;
		}

		js_free_rt(rt, s);
}

static JSValue js_server_ctor(JSContext *ctx,
                             JSValueConst new_target,
                             int argc, JSValueConst *argv)
{
    JSServerData* s = js_mallocz(ctx, sizeof(JSServerData));
    if (!s) return JS_EXCEPTION;
;
    JSValue proto = JS_UNDEFINED;
		JSValue obj = JS_UNDEFINED;
		proto = JS_GetPropertyStr(ctx, new_target, "prototype");
    if (JS_IsException(proto)) goto fail;

    obj = JS_NewObjectProtoClass(ctx, proto, js_server_class_id);
    JS_FreeValue(ctx, proto);
    if (JS_IsException(obj)) goto fail;
    JS_SetOpaque(obj, s);

    return obj;
 fail:
    js_free(ctx, s);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
}

static JSValue js_server_end(JSContext *ctx, JSValueConst this_val,int argc, JSValueConst *argv){
		int fd;
		JS_ToInt32(ctx, &fd, argv[0]);
		JSServerData *s = JS_GetOpaque2(ctx, this_val, js_server_class_id);
    if (!s){
			printf("ERROR: server opaque NULL\n");
			return JS_EXCEPTION;
		}
		shutdown(fd, SHUT_WR);
		return JS_UNDEFINED;
}

SSL_CTX *create_server_ssl_ctx( const char* key, const char* cert ) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { ERR_print_errors_fp(stderr); exit(1); }

    // Require TLS 1.2 minimum
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    // Load certificate and private key
    if (SSL_CTX_use_certificate_file(ctx, cert, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr); exit(1);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr); exit(1);
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        fprintf(stderr, "Certificate and private key do not match\n"); exit(1);
    }
    return ctx;
}

void *server_ssl_thread(void *arg) {
    ssl_thread_arg_t *args = (ssl_thread_arg_t *)arg;
    char buf[4096];
		struct pollfd fds[2];
		fds[0].fd     = SSL_get_fd( args->ssl );
		fds[0].events = POLLIN | POLLHUP | POLLERR;
		fds[1].fd     = args->fds[0];
		fds[1].events = POLLIN | POLLHUP;
    while (1) {
        int ready = poll(fds, 2, -1);
        if (ready < 0) { perror("poll"); break; }
        if (fds[0].revents & POLLIN) {
            int  n = SSL_read(args->ssl, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
						write( args->fds[1], buf, strlen( buf ) );
        }

        // --- main thread sent a message ---
        if (fds[1].revents & POLLIN) {
            int n = read(args->fds[0], buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
						SSL_write( args->ssl, buf, strlen(buf) );
        }
    }
		close( SSL_get_fd( args->ssl ) );
    SSL_shutdown(args->ssl);
    SSL_free(args->ssl);
		SSL_CTX_free(args->ctx);
		close(args->fds[1]);
		close(args->fds[0]);
    free(args);
    printf("[server_ssl_thread] exiting client fd %d\n", fds[0].fd);
    return NULL;
}

typedef struct{
    int listen_fd;
		int pipe_w_fd;
		const char* key;
		const char* cert;
} accept_thread_arg_t;

void* accept_thread_func( void* arg ){
		accept_thread_arg_t* accept_thread_arg = (accept_thread_arg_t*)arg;
		int flags = fcntl(accept_thread_arg->listen_fd, F_GETFL, 0);
    if (flags == -1) {
        printf("ERROR: listen_fd %d is invalid\n", accept_thread_arg->listen_fd);
        return NULL;
    }

		while( 1 ){
			// fds for JS r/w: https ? ssl thread pipes : client socket
			int js_fds[2];
			struct sockaddr_in client_addr;
			socklen_t client_len = sizeof(client_addr);
			if( accept_thread_arg->key && accept_thread_arg->cert ){
				int client_fd = accept(accept_thread_arg->listen_fd, (struct sockaddr *)&client_addr, &client_len);
				if( client_fd < 0 ){
					perror("accept client_fd < 0");
					continue;
				}
				//printf("[accpet_thread] TLS client connected from %s\n", inet_ntoa(client_addr.sin_addr));

				SSL_CTX *ctx = create_server_ssl_ctx( accept_thread_arg->key, accept_thread_arg->cert );
				SSL *ssl = SSL_new(ctx);
				SSL_set_fd(ssl, client_fd);

				if (SSL_accept(ssl) <= 0) {
						fprintf(stderr, "SSL_accept failed\n");
						SSL_free(ssl);
    				close(client_fd);
						continue;
				}

				create_ssl_thread( ssl, ctx, server_ssl_thread, js_fds, (void*) NULL, false );
				int bytes = write( accept_thread_arg->pipe_w_fd, js_fds, sizeof(js_fds) );
				if(bytes == -1 && errno == EPIPE) {
						printf("accept_thread_func EPIPE error on fd %d.\n", accept_thread_arg->pipe_w_fd );
						close( accept_thread_arg->pipe_w_fd );
						return NULL;
				}
			} else {
				int client_fd = accept(accept_thread_arg->listen_fd, NULL, NULL);
				//printf("[accpet_thread] client connected from %s\n", inet_ntoa(client_addr.sin_addr));
				js_fds[ 0 ] = client_fd;
				js_fds[ 1 ] = client_fd;
				int bytes = write( accept_thread_arg->pipe_w_fd, js_fds, sizeof(js_fds) );

				if(bytes == -1 && errno == EPIPE) {
						printf("accept_thread_func EPIPE error on fd %d.\n", accept_thread_arg->pipe_w_fd );
						close( accept_thread_arg->pipe_w_fd );
						return NULL;
				}
			}
		}
		printf( "accept_thread stopped fd: %d\n", accept_thread_arg->listen_fd );
		free( arg );
		return NULL;
}

static int pipefds[ 2 ];

JSValue js_stop_listen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
		printf( "accpet_thread stopping\n" );
    return JS_UNDEFINED;
}

static JSValue js_server_listen(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
		int port;
		const char* key = NULL;
		const char* cert = NULL;

		if (argc != 1 || JS_VALUE_GET_TAG(argv[0]) != JS_TAG_OBJECT) {
      	return JS_EXCEPTION;
    }

    JSValue js_port = JS_GetPropertyStr(ctx, argv[0], "port");
		if( JS_VALUE_GET_TAG( js_port ) == JS_TAG_UNDEFINED ){
			perror( "connect: { port } required" );
			JS_FreeValue(ctx, js_port);
			return JS_EXCEPTION;
		} else {
			JS_ToInt32( ctx, &port, js_port );
			JS_FreeValue( ctx, js_port );
		}

		JSValue js_key = JS_GetPropertyStr(ctx, argv[0], "key");
		if( JS_VALUE_GET_TAG( js_key ) != JS_TAG_UNDEFINED )
				key = JS_ToCString(ctx, js_key);
		JS_FreeValue(ctx, js_key);

		JSValue js_cert = JS_GetPropertyStr(ctx, argv[0], "cert");
		if( JS_VALUE_GET_TAG( js_cert ) != JS_TAG_UNDEFINED )
				cert = JS_ToCString(ctx, js_cert);
		JS_FreeValue(ctx, js_cert);

		int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
		if (listen_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

		int opt = 1;
		setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = INADDR_ANY,
    };

		if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
				perror("bind");
				close(listen_fd);
				return JS_NewInt32( ctx, -1 );
		}
    int flags = fcntl(listen_fd, F_GETFL, 0);
    if (flags == -1) {
        printf("ERROR: listen_fd %d is invalid\n", listen_fd);
        return JS_NewInt32( ctx, -1 );
    }
		if (listen(listen_fd, MAX_CLIENTS) < 0) {
				perror("listen"); close(listen_fd); exit(EXIT_FAILURE);
				return JS_UNDEFINED;
		}

		pipe(pipefds);

		accept_thread_arg_t* accept_thread_arg = malloc( sizeof( accept_thread_arg_t ) );
		accept_thread_arg->listen_fd = listen_fd;
		accept_thread_arg->pipe_w_fd = pipefds[ 1 ];
		accept_thread_arg->cert = cert;
		accept_thread_arg->key = key;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&global_accept_thread, &attr, accept_thread_func, (void*)accept_thread_arg );
    pthread_attr_destroy(&attr);

		JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "pipe_fd", JS_NewInt32(ctx, pipefds[ 0 ] ) );
    JS_SetPropertyStr(ctx, obj, "stop", JS_NewCFunction(ctx, js_stop_listen, "stop", 0));

		//printf("[Server]%s on port %d, listening on fd %d\n", cert && key ? " TLS" : "", port, listen_fd);
    return obj;
}

static JSClassDef js_server_class = {
    "Server",
    .finalizer = js_server_finalizer,
};

static const JSCFunctionListEntry js_server_proto_funcs[] = {
    JS_CFUNC_DEF("listen", 0, js_server_listen),
		JS_CFUNC_DEF("end", 0, js_server_end)
};

static int js_socket_init(JSContext *ctx, JSModuleDef *m)
{
		/* create the Client class */

    JSValue client_proto, client_class;

    /* create the Client class */
    JS_NewClassID(&js_client_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_client_class_id, &js_client_class);
    client_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, client_proto, js_client_proto_funcs, countof(js_client_proto_funcs));
    client_class = JS_NewCFunction2(ctx, js_client_ctor, "Client", 2, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, client_class, client_proto);
    JS_SetClassProto(ctx, js_client_class_id, client_proto);
    JS_SetModuleExport(ctx, m, "Client", client_class);

    /* create the Server class */
		JSValue server_proto, server_class;
    JS_NewClassID(&js_server_class_id);
    JS_NewClass(JS_GetRuntime(ctx), js_server_class_id, &js_server_class);
    server_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, server_proto, js_server_proto_funcs, countof(js_server_proto_funcs));
    server_class = JS_NewCFunction2(ctx, js_server_ctor, "Server", 2, JS_CFUNC_constructor, 0);
    JS_SetConstructor(ctx, server_class, server_proto);
    JS_SetClassProto(ctx, js_server_class_id, server_proto);
    JS_SetModuleExport(ctx, m, "Server", server_class);

		JS_SetModuleExport(ctx, m, "dispatchInit",
    JS_NewCFunction(ctx, js_dispatch_init, "dispatchInit", 0));
		JS_SetModuleExport(ctx, m, "dispatchDrain",
    JS_NewCFunction(ctx, js_dispatch_drain, "dispatchDrain", 0));

		// JS_SetModuleExport(ctx, m, "someFunction",
    // JS_NewCFunction(ctx, js_some_function, "someFunction", 1));  // 1 = expected arg count

    return 0;
}

JSModuleDef *JS_INIT_MODULE(JSContext *ctx, const char *module_name)
{
		struct sigaction sa = {
        .sa_handler = sigint_handler,
        .sa_flags   = 0,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    JSModuleDef *m;
    m = JS_NewCModule(ctx, module_name, js_socket_init);
    if (!m)
        return NULL;
    JS_AddModuleExport(ctx, m, "Client");
		JS_AddModuleExport(ctx, m, "Server");
		JS_AddModuleExport(ctx, m, "dispatchInit");
		JS_AddModuleExport(ctx, m, "dispatchDrain");
		// JS_AddModuleExport(ctx, m, "someFunction"); // example of exporting a function
    return m;
}
