#ifndef UART_BRIDGE_H
#define UART_BRIDGE_H

#include <stdint.h>

#define UART_RF_PAYLOAD_LEN 64
#define UART_RF_MAX_DATA    60

int uart_bridge_init(const char *dev_path, int baud);
void uart_bridge_close(void);

/* Leser fra UART og legger ferdige RF-payloads i intern kø */
int uart_bridge_poll_input(void);

/* Henter neste ferdige payload som skal sendes over RF.
   Returnerer 1 hvis en payload ble hentet, ellers 0. */
int uart_bridge_get_tx_packet(uint8_t out[UART_RF_PAYLOAD_LEN]);

/* Tar imot en RF-payload og skriver den ut på UART.
   Returnerer antall bytes skrevet, eller -1 ved feil. */
int uart_bridge_write_rf_payload(const uint8_t payload[UART_RF_PAYLOAD_LEN]);

#endif