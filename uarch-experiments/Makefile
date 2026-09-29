CFLAGS = -g -O3

ARCH = undef

all: main
.PHONY: clean

main: main.c test_rsb.c
	gcc -o $@ main.c test_rsb.c $(CFLAGS) -D$(ARCH)

clean:
	rm -f main.o
	rm -f libfr.so

so: main.c
	gcc -fPIC -shared -o libfr.so main.c $(CFLAGS) -D$(ARCH)
