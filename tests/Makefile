# SPDX-License-Identifier: GPL-2.0-or-later
#
# Host unit test for the USIF baud divider search (src/usif_baud.c).
#   make -C tests          build and run
#
CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -Wall -Wextra -Werror -I../src

all: run

test_baud: test_baud.c ../src/usif_baud.c ../src/usif_baud.h
	$(CC) $(CFLAGS) -o $@ test_baud.c ../src/usif_baud.c

run: test_baud
	./test_baud

clean:
	rm -f test_baud

.PHONY: all run clean
