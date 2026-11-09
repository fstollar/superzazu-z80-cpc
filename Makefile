bin = z80_tests
src = $(filter-out z80_tstate_test.c, $(wildcard *.c))
obj = $(src:.c=.o)
CFLAGS = -g -Wall -Wextra -O2 -std=c99 -pedantic
LDFLAGS =

.PHONY: all clean tstate_test

all: $(bin)

$(bin): $(obj)
	$(CC) -o $@ $^ $(LDFLAGS)

# Bus-cycle timing test: kept out of src (it has its own main and needs
# the new T counter interface), built on demand only.
tstate_test: z80_tstate_test.o z80.o z80_timing_generic.o
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	-rm $(bin) tstate_test $(obj) z80_tstate_test.o
