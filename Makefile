all: jobhog

CFLAGS += -Wall -Wextra -Werror -g

jobhog: jobhog.c
	$(CC) $(CFLAGS) -o $@ $<

test: Makefile.test all
	make -f $< -j1234 test

.PHONY: all test
