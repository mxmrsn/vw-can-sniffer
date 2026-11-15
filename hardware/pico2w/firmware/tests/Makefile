CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

test: test_isotp_uds
	./test_isotp_uds

test_isotp_uds: test_isotp_uds.c ../src/isotp.c ../src/uds.c ../src/isotp.h ../src/uds.h
	$(CC) $(CFLAGS) -o $@ test_isotp_uds.c ../src/isotp.c ../src/uds.c

clean:
	rm -rf test_isotp_uds test_isotp_uds.dSYM

.PHONY: test clean
