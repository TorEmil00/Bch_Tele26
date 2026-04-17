#define _GNU_SOURCE
#include "uart_bridge.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <termios.h>
#include <stdlib.h>

#define UART_QUEUE_DEPTH 16

static int uart_fd = -1;
static uint8_t tx_queue[UART_QUEUE_DEPTH][UART_RF_PAYLOAD_LEN];
static int q_head = 0;
static int q_tail = 0;
static int q_count = 0;
static uint8_t uart_seq = 0;
static pthread_mutex_t q_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint8_t calculate_crc8_local(const uint8_t *data, size_t len)
{
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ?
            (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

static speed_t map_baud(int baud)
{
    switch (baud) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B115200;
    }
}

static int queue_push(const uint8_t payload[UART_RF_PAYLOAD_LEN])
{
    pthread_mutex_lock(&q_mutex);
    if (q_count >= UART_QUEUE_DEPTH) {
        pthread_mutex_unlock(&q_mutex);
        return 0;
    }

    memcpy(tx_queue[q_tail], payload, UART_RF_PAYLOAD_LEN);
    q_tail = (q_tail + 1) % UART_QUEUE_DEPTH;
    q_count++;

    pthread_mutex_unlock(&q_mutex);
    return 1;
}

int uart_bridge_get_tx_packet(uint8_t out[UART_RF_PAYLOAD_LEN])
{
    pthread_mutex_lock(&q_mutex);

    if (q_count == 0) {
        pthread_mutex_unlock(&q_mutex);
        return 0;
    }

    memcpy(out, tx_queue[q_head], UART_RF_PAYLOAD_LEN);
    q_head = (q_head + 1) % UART_QUEUE_DEPTH;
    q_count--;

    pthread_mutex_unlock(&q_mutex);
    return 1;
}

int uart_bridge_init(const char *dev_path, int baud)
{
    struct termios tio;
    speed_t spd;
    if (!dev_path)
        return -1;

    uart_fd = open(dev_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (uart_fd < 0) {
        perror("open uart");
        return -1;
    }

    memset(&tio, 0, sizeof(tio));
    cfmakeraw(&tio); // Sikrer at Linux ikke roter med dataene

    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CRTSCTS;

    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_lflag = 0;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    spd = map_baud(baud);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    tcflush(uart_fd, TCIOFLUSH);
    if (tcsetattr(uart_fd, TCSANOW, &tio) != 0) {
        perror("tcsetattr");
        close(uart_fd);
        uart_fd = -1;
        return -1;
    }

    return 0;
}

void uart_bridge_close(void)
{
    if (uart_fd >= 0) {
        close(uart_fd);
        uart_fd = -1;
    }
}

int uart_bridge_poll_input(void)
{
    if (uart_fd < 0)
        return -1;
    int packets_created = 0;

    while (1) {
        uint8_t buf[UART_RF_MAX_DATA];
        ssize_t n = read(uart_fd, buf, sizeof(buf));

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            perror("uart read");
            return -1;
        }

        if (n == 0)
            break;
        uint8_t payload[UART_RF_PAYLOAD_LEN];
        memset(payload, 0, sizeof(payload));

        /* Format:
           [0]      = len
           [1..60]  = data
           [61]     = reserved
           [62]     = seq
           [63]     = crc8 over [0..62]
        */

        payload[0] = (uint8_t)n;
        memcpy(&payload[1], buf, (size_t)n);
        payload[61] = 0;
        payload[62] = uart_seq++;
        payload[63] = calculate_crc8_local(payload, UART_RF_PAYLOAD_LEN - 1);

        if (queue_push(payload))
            packets_created++;
    }

    return packets_created;
}

int uart_bridge_write_rf_payload(const uint8_t payload[UART_RF_PAYLOAD_LEN])
{
    if (uart_fd < 0)
        return -1;
    if (!payload)
        return -1;

    uint8_t crc = calculate_crc8_local(payload, UART_RF_PAYLOAD_LEN - 1);
    if (crc != payload[UART_RF_PAYLOAD_LEN - 1]) {
        // FJERNET: Vi printer ikke lenger CRC-feil til skjermen. 
        // Det er normalt med litt støy på RF, vi bare kaster den ødelagte pakken i stillhet!
        return -1;
    }

    uint8_t len = payload[0];
    if (len > UART_RF_MAX_DATA) {
        // Ignorerer pakker som påstår de er for lange (støy kan endre lengde-byten)
        return -1;
    }

    size_t written_total = 0;
    int retry_count = 0; // NYTT: Teller for å hindre evig loop

    while (written_total < len) {
        ssize_t w = write(uart_fd, &payload[1] + written_total, len - written_total);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Hvis UART-pinnen er oversvømt, prøver vi maks 5 ganger.
                // Er den fortsatt full, kaster vi pakken i stedet for å fryse hele raketten!
                if (retry_count++ > 5) break; 
                usleep(1000);
                continue;
            }
            return -1;
        }
        written_total += (size_t)w;
    }

    // FJERNET: tcdrain(uart_fd); //<--- Synderen som låste hele programmet er nå borte!
    
    return (int)written_total;
}