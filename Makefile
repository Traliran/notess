CC      ?= gcc
TARGET  := notess
SRC     := main.c cache.c
HEADERS := cache.h

CFLAGS   += -O2 -Wall -Wextra -Wpedantic -std=c11
GTKFLAGS := $(shell pkg-config --cflags gtk4 libcurl)
LDLIBS   := $(shell pkg-config --libs gtk4 libcurl) -lpthread

PREFIX   ?= /usr/local
BINDIR   := $(PREFIX)/bin

all: $(TARGET)

$(TARGET): $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(GTKFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

.PHONY: all clean install uninstall
