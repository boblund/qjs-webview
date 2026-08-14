QJSC = /usr/local/bin/qjsc
CC = clang
CXX = clang++

CFLAGS = -O2 -g -Wall -fPIC  -I/usr/local/include/quickjs  -I.
CXXFLAGS = -std=c++11 -DWEBVIEW_COCOA -I.
LDFLAGS = \
	-L/usr/local/lib/quickjs -lquickjs \
	-L. -lqjswebview \
	-framework WebKit -framework Cocoa

TARGETS = webviewApp

all: $(TARGETS)

impl.o: impl.cc webview.h
	$(CXX) -c impl.cc $(CXXFLAGS) -o impl.o

 js_dispatch.o: js_dispatch.c js_dispatch.h
	$(CC) $(CFLAGS) -c js_dispatch.c -o js_dispatch.o

# webview
webview.o: webview.c webview.h js_dispatch.h
	$(CC) $(CFLAGS) -c webview.c -o webview.o

libqjswebview.a: webview.o js_dispatch.o impl.o
	ar rcs $@ $^

webview.so: webview.o impl.o
	$(CXX) -shared -o webview.so webview.o impl.o -framework WebKit -framework Cocoa

webviewApp.c: webviewApp.mjs
	$(QJSC) -e -M webview.so,webview -o webviewApp.c webviewApp.mjs

webviewApp.o: webviewApp.c
	$(CC) $(CFLAGS) -c webviewApp.c -o webviewApp.o

webviewApp: webviewApp.o  libqjswebview.a
	$(CC) -lc++ webviewApp.o webview.o $(LDFLAGS) -o webviewApp

.PHONY: clean

clean:
	rm -f *.o webviewApp.[co] webviewApp
