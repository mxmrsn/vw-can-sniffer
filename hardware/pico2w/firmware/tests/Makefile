CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

test: test_isotp_uds test_frame test_logger
	./test_isotp_uds
	./test_frame
	./test_logger

test_isotp_uds: test_isotp_uds.c ../src/isotp.c ../src/uds.c ../src/isotp.h ../src/uds.h
	$(CC) $(CFLAGS) -o $@ test_isotp_uds.c ../src/isotp.c ../src/uds.c

test_frame: test_frame.c ../src/frame.c ../src/frame.h
	$(CC) $(CFLAGS) -o $@ test_frame.c ../src/frame.c

test_logger: test_logger.c ../src/logger.c ../src/logger.h ../src/storage.h
	$(CC) $(CFLAGS) -o $@ test_logger.c ../src/logger.c

clean:
	rm -rf test_isotp_uds test_frame test_logger *.dSYM

.PHONY: test clean
