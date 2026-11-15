CFLAGS = -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O1 -g
SRC = main_host.c sim_bus.c ../src/app.c ../src/isotp.c ../src/uds.c

vw_diag_sim: $(SRC) $(wildcard ../src/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	rm -rf vw_diag_sim vw_diag_sim.dSYM

.PHONY: clean
