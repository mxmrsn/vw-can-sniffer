CFLAGS = -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

test: test_isotp_uds test_frame test_logger test_ble_link test_power
	./test_isotp_uds
	./test_frame
	./test_logger
	./test_ble_link
	./test_power

test_isotp_uds: test_isotp_uds.c ../src/isotp.c ../src/uds.c ../src/isotp.h ../src/uds.h
	$(CC) $(CFLAGS) -o $@ test_isotp_uds.c ../src/isotp.c ../src/uds.c

test_frame: test_frame.c ../src/frame.c ../src/frame.h
	$(CC) $(CFLAGS) -o $@ test_frame.c ../src/frame.c

test_logger: test_logger.c ../src/logger.c ../src/logger.h ../src/storage.h
	$(CC) $(CFLAGS) -o $@ test_logger.c ../src/logger.c

test_ble_link: test_ble_link.c ../src/ble_link.c ../src/ble_link.h
	$(CC) $(CFLAGS) -o $@ test_ble_link.c ../src/ble_link.c

test_power: test_power.c ../src/power.c ../src/power.h
	$(CC) $(CFLAGS) -o $@ test_power.c ../src/power.c

clean:
	rm -rf test_isotp_uds test_frame test_logger test_ble_link test_power *.dSYM

.PHONY: test clean
