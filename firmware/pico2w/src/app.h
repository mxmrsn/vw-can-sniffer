// Application core: serial console, target selection, UDS commands.
// Platform independent; see platform.h and can_port.h for what it needs.
#pragma once

void app_init(void);
void app_poll(void); // call continuously from the main loop
