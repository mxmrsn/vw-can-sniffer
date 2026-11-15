// Simulator stand-in for Bluetooth LE: a TCP server on 127.0.0.1 that behaves
// like the board's Nordic UART Service, so the binary protocol and a phone app
// (e.g. in the iOS Simulator) can be tested without hardware.
//
//   VWDIAG_BLE_PORT=<port>   enable it (one client at a time)
//   VWDIAG_BLE_MTU=<bytes>   ATT MTU to imitate, default 185 (iOS); pieces are MTU - 3
//   VWDIAG_BLE_RATE=<B/s>    throughput to imitate, default 20000
//
// A connected client counts as paired and subscribed; there is no pairing step.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/ble_link.h"
#include "../src/platform.h"

static int listener = -1, client = -1;
static unsigned mtu = 185, rate = 20000;
static double budget;
static uint32_t last_ms;
static int started;

static void start(void)
{
    started = 1;
    const char *port = getenv("VWDIAG_BLE_PORT");
    if (!port || !*port)
        return;
    if (getenv("VWDIAG_BLE_MTU"))
        mtu = (unsigned)atoi(getenv("VWDIAG_BLE_MTU"));
    if (getenv("VWDIAG_BLE_RATE"))
        rate = (unsigned)atoi(getenv("VWDIAG_BLE_RATE"));
    if (mtu < 23)
        mtu = 23;

    listener = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port)) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (bind(listener, (struct sockaddr *)&a, sizeof(a)) || listen(listener, 1)) {
        fprintf(stderr, "ble emulation: cannot listen on port %s: %s\n", port, strerror(errno));
        close(listener);
        listener = -1;
        return;
    }
    fcntl(listener, F_SETFL, O_NONBLOCK);
    ble_info.available = true;
    ble_info.emulated = true;
    snprintf(ble_info.name, sizeof(ble_info.name), "VW-Diag-SIM");
    snprintf(ble_info.address, sizeof(ble_info.address), "127.0.0.1:%s", port);
}

static void drop_client(void)
{
    close(client);
    client = -1;
    ble_info.connected = ble_info.subscribed = false;
    ble_link_tx_discard();
    ble_link_new_session();
}

void plat_ble_poll(void)
{
    if (!started)
        start();
    if (listener < 0)
        return;
    uint32_t now = plat_now_ms();
    if (client < 0) {
        client = accept(listener, NULL, NULL);
        if (client < 0)
            return;
        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        fcntl(client, F_SETFL, O_NONBLOCK);
        ble_link_tx_discard();
        ble_link_new_session();
        ble_info.connected = ble_info.subscribed = true;
        ble_info.mtu = (uint16_t)mtu;
        ble_info.connections++;
        budget = 0;
        last_ms = now;
    }

    // Writes from the "phone", at most one ATT write's worth at a time.
    uint8_t buf[512];
    size_t piece = mtu - 3 < sizeof(buf) ? mtu - 3 : sizeof(buf);
    for (int i = 0; i < 16; i++) {
        ssize_t n = read(client, buf, piece);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            drop_client();
            return;
        }
        if (n < 0)
            break;
        ble_link_rx_push(buf, (size_t)n);
    }

    // Notifications, limited to the imitated throughput.
    budget += (double)(now - last_ms) * rate / 1000.0;
    last_ms = now;
    if (budget > 4.0 * piece)
        budget = 4.0 * piece;
    while (budget >= 1 && ble_link_tx_pending()) {
        size_t want = budget < piece ? (size_t)budget : piece;
        if (want < piece && ble_link_tx_pending() > want)
            break; // wait until a whole notification's worth of budget is there
        size_t n = ble_link_tx_take(buf, want);
        budget -= (double)n;
        for (size_t off = 0; off < n;) {
            ssize_t w = write(client, buf + off, n - off);
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                usleep(100);
                continue;
            }
            if (w <= 0) {
                drop_client();
                return;
            }
            off += (size_t)w;
        }
    }
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
