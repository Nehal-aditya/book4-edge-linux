CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wno-unused-parameter
CFLAGS  += $(shell pkg-config --cflags libdrm)
LDLIBS  += $(shell pkg-config --libs libdrm)

all: book4-splash

book4-splash: book4-splash.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# No `make install`: the units must go in together with the sddm drop-in that
# tells the daemon to release the display, or the greeter never gets it. Use install.sh.
clean:
	rm -f book4-splash

.PHONY: all clean
