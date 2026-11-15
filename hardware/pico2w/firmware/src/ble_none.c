// Builds for a Pico without a radio (Pico, Pico 2): Bluetooth is simply absent.
#include "ble_link.h"
#include "platform.h"

void ble_pico_init(void)
{
}

void plat_ble_poll(void)
{
}

void plat_ble_kick(void)
{
}

bool plat_ble_open_pairing(void)
{
    return false;
}

bool plat_ble_forget(void)
{
    return false;
}
