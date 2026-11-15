CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

test: test_isotp_uds test_frame
	./test_isotp_uds
	./test_frame

test_isotp_uds: test_isotp_uds.c ../src/isotp.c ../src/uds.c ../src/isotp.h ../src/uds.h
	$(CC) $(CFLAGS) -o $@ test_isotp_uds.c ../src/isotp.c ../src/uds.c

test_frame: test_frame.c ../src/frame.c ../src/frame.h
	$(CC) $(CFLAGS) -o $@ test_frame.c ../src/frame.c

clean:
	rm -rf test_isotp_uds test_frame *.dSYM

.PHONY: test clean
