PKGS = wayland-client wayland-egl egl glesv2 gbm libdrm json-c
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE \
          $(shell pkg-config --cflags $(PKGS)) -Igen
LDLIBS = $(shell pkg-config --libs $(PKGS))

WP = /usr/share/wayland-protocols
XMLS = $(WP)/stable/xdg-shell/xdg-shell.xml \
       $(WP)/stable/linux-dmabuf/linux-dmabuf-v1.xml \
       protocol/wlr-screencopy-unstable-v1.xml

GEN_C = $(patsubst %.xml,gen/%-protocol.c,$(notdir $(XMLS)))
GEN_H = $(patsubst %.xml,gen/%-client-protocol.h,$(notdir $(XMLS)))
SRC = src/main.c src/capture.c src/render.c src/sway.c
OBJ = $(SRC:src/%.c=gen/%.o) $(GEN_C:.c=.o)

glasses-presenter: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDLIBS)

# locate each protocol xml by basename
vpath %.xml $(WP)/stable/xdg-shell $(WP)/stable/linux-dmabuf protocol

gen/%-protocol.c: %.xml | gen
	wayland-scanner private-code $< $@
gen/%-client-protocol.h: %.xml | gen
	wayland-scanner client-header $< $@

gen/%.o: gen/%.c
	$(CC) $(CFLAGS) -c -o $@ $<
gen/%.o: src/%.c $(GEN_H)
	$(CC) $(CFLAGS) -c -o $@ $<

gen:
	mkdir -p gen

PREFIX ?= /usr/local
install: glasses-presenter
	install -Dm755 glasses-presenter $(DESTDIR)$(PREFIX)/bin/glasses-presenter
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/glasses-presenter
clean:
	rm -rf gen glasses-presenter

.PHONY: install uninstall clean
