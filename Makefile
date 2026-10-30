CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Werror -pedantic
BUILD   := build
TESTS   := zfifo zfifo_2n

.PHONY: test test-tsan clean

test: $(TESTS:%=$(BUILD)/test_%)
	@for t in $^; do ./$$t || exit 1; done

test-tsan: $(TESTS:%=$(BUILD)/test_%_tsan)
	@for t in $^; do ./$$t || exit 1; done

$(BUILD)/test_%: %.c %.h zfifo_port.h test/test_%.c | $(BUILD)
	$(CC) $(CFLAGS) -I. $*.c test/test_$*.c -lpthread -o $@

$(BUILD)/test_%_tsan: %.c %.h zfifo_port.h test/test_%.c | $(BUILD)
	$(CC) $(CFLAGS) -g -fsanitize=thread -I. $*.c test/test_$*.c -lpthread -o $@

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)
