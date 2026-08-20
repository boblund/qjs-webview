QJSC = /usr/local/bin/qjsc
CC = clang
CXX = clang++

CFLAGS = -O2 -g -Wall -fPIC  -I/usr/local/include/quickjs -I/opt/homebrew/opt/openssl/include  -I.
CXXFLAGS = -std=c++11 -DWEBVIEW_COCOA -I.
LDFLAGS = \
	-L/usr/local/lib/quickjs -lquickjs \
	-L/opt/homebrew/opt/openssl/lib -lssl -lcrypto \
	-L/usr/local/lib/libdatachannel -ldatachannel -lusrsctp -ljuice \
	-L. -lqjsdc \
	-L. -lqjswebview \
	-framework WebKit -framework Cocoa

TARGETS = webviewApp

all: $(TARGETS)

impl.o: impl.cc webview.h
	$(CXX) -c impl.cc $(CXXFLAGS) -o impl.o

js_dispatch.o: js_dispatch.c js_dispatch.h
	$(CC) $(CFLAGS) -c js_dispatch.c -o js_dispatch.o

# socket
socket.o: socket.c
	$(CC) $(CFLAGS) -c socket.c -o socket.o

# webview
webview.o: webview.c webview.h js_dispatch.h
	$(CC) $(CFLAGS) -c webview.c -o webview.o

libqjswebview.a: webview.o js_dispatch.o impl.o
	ar rcs $@ $^

webview.so: webview.o impl.o
	$(CXX) -shared -o webview.so webview.o impl.o -framework WebKit -framework Cocoa

webviewApp.c: webviewApp.mjs
	$(QJSC) -e -M webview.so,webview -M socket.so,socket -M dc.so,dc -o webviewApp.c webviewApp.mjs

webviewApp.o: webviewApp.c
	$(CC) $(CFLAGS) -c webviewApp.c -o webviewApp.o

webviewApp: webviewApp.o socket.o js_dispatch.o
	$(CC) -lc++ webviewApp.o socket.o js_dispatch.o $(LDFLAGS) -o webviewApp

.PHONY: clean

clean:
	rm -f *.o webviewApp.[co] webviewApp
