CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags x11)
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -pthread
LDLIBS += $(shell $(PKG_CONFIG) --libs x11) -ldl -pthread
SOURCES = src/main.c src/sensors.c src/pixel.c

.PHONY: all clean install uninstall test
all: wmpixeltemp
wmpixeltemp: $(SOURCES) src/sensors.h src/pixel.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)
install: wmpixeltemp
	install -Dm755 wmpixeltemp $(DESTDIR)$(BINDIR)/wmpixeltemp
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/wmpixeltemp
test: wmpixeltemp
	python3 tests/test_sensors.py
clean:
	rm -f wmpixeltemp
