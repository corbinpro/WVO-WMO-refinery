# refinery-ui: build on the Pi (needs gcc, libjpeg62-turbo-dev, zlib1g-dev)
CC      ?= gcc
ARCH    := $(shell uname -m)
CFLAGS  ?= -O2
CFLAGS  += -std=gnu99 -Wall -Wextra -D_GNU_SOURCE
ifeq ($(ARCH),armv7l)
CFLAGS  += -mcpu=cortex-a7 -mfpu=neon-vfpv4
endif
LDLIBS  = -ljpeg -lz

SRCS = main.c link.c fb.c input.c ui.c web.c cam.c
OBJS = $(SRCS:.c=.o)

refinery-ui: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJS): refinery-ui.h
web.o: index_html.h

# Embed web/index.html as a C string
index_html.h: web/index.html
	{ echo 'static const char index_html[] ='; \
	  sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $<; echo ';'; } > $@

install: refinery-ui
	install -m 755 refinery-ui /usr/local/bin/refinery-ui

clean:
	rm -f refinery-ui $(OBJS) index_html.h

.PHONY: install clean
