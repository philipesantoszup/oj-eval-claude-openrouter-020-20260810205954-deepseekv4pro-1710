.PHONY: all clean

all: code

code: main.c buddy.c buddy.h utils.h
	gcc -std=c11 -O2 -Wall -o code main.c buddy.c

clean:
	rm -f code test