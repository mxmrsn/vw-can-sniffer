CFLAGS = -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O1 -g
SRC = main_host.c sim_bus.c car.c sim_config.c sim_engine.c storage_host.c ble_host.c ../src/ble_link.c ../src/logger.c ../src/app.c ../src/isotp.c ../src/uds.c ../src/frame.c ../src/proto.c

vw_diag_sim: $(SRC) $(wildcard ../src/*.h) sim.h
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -rf vw_diag_sim vw_diag_sim.dSYM

.PHONY: clean
