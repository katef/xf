
CAIRO_FLAGS      != pkg-config --libs --cflags cairo
X11_FLAGS        != pkg-config --libs --cflags x11
PANGOCAIRO_FLAGS != pkg-config --libs --cflags pangocairo
FLEX_FLAGS       ?= -I ../share/git/flex ../share/git/flex/flex.c

a.out: main.c
	gcc -std=c99 -Wall ${CAIRO_FLAGS} ${X11_FLAGS} ${PANGOCAIRO_FLAGS} ${FLEX_FLAGS} -o $@ main.c

