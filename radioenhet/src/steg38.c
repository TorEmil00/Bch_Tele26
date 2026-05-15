/*
 * steg38.c — Rakett-side firmware for SDR-telemetrilenken.
 *
 * Kjører på Pluto/Zynq SoC under Linux. Tre POSIX-tråder håndterer
 * sender (tx_thread), mottaker (rx_thread) og periodisk telemetri-
 * innsamling (telemetry_thread). TX og RX pinnes til hver sin CPU
 * for å holde sanntidskravene i signalbehandlingen.
 *
 * Sender 868 MHz (telemetri til bakke), mottar 433 MHz (kommandoer
 * fra bakke). BPSK med IIR-pulsforming, 50 sampler per symbol ved
 * 2,5 MS/s. Pakkeformat: 32-bit synkord + 64-byte hvitnet payload
 * + CRC-8 (polynom 0x07).
 */

#define _GNU_SOURCE
#include "uart_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <iio.h>
#include <liquid/liquid.h>


/* ----------------------------------------------------------------
 * Global flagg for ren avslutning ved SIGINT (Ctrl+C).
 * ---------------------------------------------------------------- */
volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }


/* ----------------------------------------------------------------
 * Hjelpe-struktur for å lese CPU-bruk fra /proc/stat.
 * ---------------------------------------------------------------- */
typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq;
} cpu_stat_t;


/* ----------------------------------------------------------------
 * Delte systemflagg som styres av apply_cmd() i RX-tråden og leses
 * av TX-tråden og telemetri-tråden. volatile fordi flere tråder
 * leser/skriver. Enkeltord-skrivinger er atomære på Cortex-A9, så
 * ingen mutex er nødvendig.
 * ---------------------------------------------------------------- */
volatile float sys_alpha = 0.60f;       // IIR-pulsforming filterkonstant (cmd id=1)
volatile long long sys_rx_gain = 40;    // RX-gain (kun bokføring; AGC styrer egentlig)
volatile long long sys_tx_atten = 15;   // TX-demping i dB (cmd id=3)
volatile int sys_tx_ampl = 15000;       // TX-amplitude som skaleres til int16 (cmd id=4)

/* Telemetri-felter oppdatert av telemetry_thread_func. */
volatile float sys_c0_load = 0.0f;
volatile float sys_c1_load = 0.0f;
volatile float sys_radio_temp = 0.0f;
volatile float sys_rssi = -100.0f;

/* Tellere som bakkesiden bruker som passive kvitteringer. */
volatile int cmd_ack_count = 0;
volatile int uart_ack_count = 0;
volatile int last_cmd_id = 0;
volatile int last_cmd_val = 0;


/* ----------------------------------------------------------------
 * Globale IIO-håndtak (settes opp i main(), brukes av alle tråder).
 * ---------------------------------------------------------------- */
struct iio_device *phy_dev_global;
struct iio_channel *rx_chan_global;
struct iio_channel *tx_chan_global;
struct iio_context *ctx;
struct iio_device *tx_dev, *rx_dev;
struct iio_channel *tx0_i, *tx0_q, *rx0_i, *rx0_q;


/* Beregner CPU-last (%) basert på diff */
float calc_cpu_load(cpu_stat_t *prev, cpu_stat_t *curr) {
    unsigned long long p_idle = prev->idle + prev->iowait;
    unsigned long long c_idle = curr->idle + curr->iowait;
    unsigned long long p_non_idle = prev->user + prev->nice + prev->system + prev->irq + prev->softirq;
    unsigned long long c_non_idle = curr->user + curr->nice + curr->system + curr->irq + curr->softirq;
    unsigned long long p_total = p_idle + p_non_idle;
    unsigned long long c_total = c_idle + c_non_idle;
    if (c_total - p_total == 0) return 0.0f;
    return (float)((c_total - p_total) - (c_idle - p_idle)) / (c_total - p_total) * 100.0f;
}

/* Henter cpu0- og cpu1-last. */
void get_core_loads(float *load0, float *load1) {
    static cpu_stat_t prev0 = {0}, prev1 = {0};
    cpu_stat_t curr0 = {0}, curr1 = {0};
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), fp)) {
        if (strncmp(buffer, "cpu0 ", 5) == 0)
            sscanf(buffer, "cpu0 %llu %llu %llu %llu %llu %llu %llu",
                   &curr0.user, &curr0.nice, &curr0.system, &curr0.idle,
                   &curr0.iowait, &curr0.irq, &curr0.softirq);
        else if (strncmp(buffer, "cpu1 ", 5) == 0)
            sscanf(buffer, "cpu1 %llu %llu %llu %llu %llu %llu %llu",
                   &curr1.user, &curr1.nice, &curr1.system, &curr1.idle,
                   &curr1.iowait, &curr1.irq, &curr1.softirq);
    }
    fclose(fp);
    *load0 = calc_cpu_load(&prev0, &curr0);
    *load1 = calc_cpu_load(&prev1, &curr1);
    prev0 = curr0;
    prev1 = curr1;
}

/* Skriver lokal tid som HH:MM:SS-streng til buffer. */
void get_ts(char *buffer, size_t len) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    snprintf(buffer, len, "%02d:%02d:%02d", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

/* CRC-8 med polynom 0x07. Samme som bakkesiden bruker. */
uint8_t calculate_crc8(uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

/* Spektral utjevning: XOR med deterministisk sekvens. Bakkesiden
 * påfører samme sekvens for å reversere maskeringen. */
void whiten_payload(uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) data[i] ^= (uint8_t)((i * 211 + 79) & 0xFF);
}

/*
 * apply_cmd: oppdaterer ett av fire systemflagg basert på kommando-id.
 *   id = 1: alpha (IIR-pulsforming)
 *   id = 2: RX-gain (kun bokføring; AGC styrer i hardware)
 *   id = 3: TX-demping (skrives også til AD9363)
 *   id = 4: TX-amplitude
 * Ukjente id-verdier oppdaterer kun ack-tellerne, og brukes av
 * bakkesidens ping/echo (id = 99).
 */
void apply_cmd(uint8_t id, uint8_t val) {
    if (id == 1) {
        sys_alpha = val / 255.0f;
        if (sys_alpha < 0.05f) sys_alpha = 0.05f;
    } else if (id == 2) {
        sys_rx_gain = val > 75 ? 75 : val;
    } else if (id == 3) {
        sys_tx_atten = val > 80 ? 80 : val;
        if (tx_chan_global) iio_channel_attr_write_longlong(tx_chan_global, "hardwaregain", sys_tx_atten);
    } else if (id == 4) {
        sys_tx_ampl = val * 128;
    }
    last_cmd_id = id; last_cmd_val = val; cmd_ack_count++;
}


/* ----------------------------------------------------------------
 * Pakkeformat og synkord.
 * TELEMETRY_SYNC er CCSDS attached sync marker. INV-variantene
 * håndterer 180° fasetvetydighet etter Costas-låsing.
 * ---------------------------------------------------------------- */
#define PAYLOAD_LEN 64
#define SAMPLES_PER_SYMBOL_TX 50
#define SAMPLES_PER_SYMBOL_RX 2
#define DECIMATION_FACTOR 25
#define TX_BUFFER_SAMPLES 65536
#define RX_BUFFER_SAMPLES 10000
#define TELEMETRY_SYNC 0x1ACFFC1D
#define COMMAND_SYNC   0x55AA55AA
#define COMMAND_SYNC_INV 0xAA55AA55
#define UART_SYNC      0xA55A3CC3
#define UART_SYNC_INV  0x5AA5C33C


/* Hvitner payload og videresender til UART-broen mot RIU. */
static int rf_uart_out(uint8_t *p) {
    whiten_payload(p, PAYLOAD_LEN);
    return uart_bridge_write_rf_payload(p);
}


/*
 * Telemetri-tråd: leser CPU-last, transceiver-temperatur, RSSI og
 * RX-gain hvert sekund. Verdiene legges i delte globaler som TX-tråden
 * pakker inn i neste telemetri-frame.
 */
void *telemetry_thread_func(void *arg) {
    (void)arg;
    while (keep_running) {
        float c0, c1;
        get_core_loads(&c0, &c1);
        sys_c0_load = c0; sys_c1_load = c1;

        long long t_val, r_val, g_val;
        struct iio_channel *tc = iio_device_find_channel(phy_dev_global, "temp0", false);
        if (tc && iio_channel_attr_read_longlong(tc, "input", &t_val) == 0) sys_radio_temp = t_val / 1000.0f;

        if (rx_chan_global && iio_channel_attr_read_longlong(rx_chan_global, "rssi", &r_val) == 0) sys_rssi = r_val;

        if (rx_chan_global && iio_channel_attr_read_longlong(rx_chan_global, "hardwaregain", &g_val) == 0) sys_rx_gain = g_val;

        sleep(1);
    }
    return NULL;
}


/*
 * TX-tråd (CPU 0): holder en IIO-buffer mot AD9363 åpen og fyller
 * den kontinuerlig. Bygger en ny 544-bit pakke ved starten av hver
 * syklus (UART hvis det er en pakke i køen, ellers telemetri).
 * Hver bit pulsformes med ett-pols IIR og skrives ut som 50 IQ-sampler.
 */
void *tx_thread_func(void *arg) {
    (void)arg; sleep(1);                          // vent på at RX-tråd og UART-bro er oppe
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, TX_BUFFER_SAMPLES, false);
    if (!txbuf) return NULL;

    uint8_t payload[PAYLOAD_LEN], tx_payload[PAYLOAD_LEN], packet_bits[544];
    uint32_t current_sync = TELEMETRY_SYNC;
    int bit_idx = 0, sample_count = 0;
    float current_tx_val = 0.0f;

    while (keep_running) {
        void *p_dat = iio_buffer_first(txbuf, tx0_i), *p_end = iio_buffer_end(txbuf);
        ptrdiff_t p_inc = iio_buffer_step(txbuf);

        for (void *p = p_dat; p < p_end; p += p_inc) {
            // Ny pakke når både sample- og bit-telleren er null.
            if (sample_count == 0 && bit_idx == 0) {
                uart_bridge_poll_input();
                memset(payload, 0, sizeof(payload));
                memset(tx_payload, 0, sizeof(tx_payload));

                if (uart_bridge_get_tx_packet(payload)) {
                    // UART-pakke fra RIU har prioritet over telemetri.
                    current_sync = UART_SYNC;
                    memcpy(tx_payload, payload, PAYLOAD_LEN);
                } else {
                    // Bygg lokal telemetripakke med snprintf og legg til CRC.
                    current_sync = TELEMETRY_SYNC;
                    float avg_cpu = (sys_c0_load + sys_c1_load) / 2.0f;
                    snprintf((char *)payload, PAYLOAD_LEN,
                             "L:%d|V:%d|K:%d|UA:%d|R:%.1f|C:%02.0f|T:%.1f|G:%lld|A:%lld",
                             last_cmd_id, last_cmd_val, cmd_ack_count, uart_ack_count,
                             sys_rssi, avg_cpu, sys_radio_temp, sys_rx_gain, sys_tx_atten);
                    payload[PAYLOAD_LEN - 1] = calculate_crc8(payload, PAYLOAD_LEN - 1);
                    memcpy(tx_payload, payload, PAYLOAD_LEN);
                }
                whiten_payload(tx_payload, PAYLOAD_LEN);

                // Serialiser ramme til 544 bit (32 sync + 64 byte × 8), MSB først.
                int temp_idx = 0;
                for (int b = 31; b >= 0; b--) packet_bits[temp_idx++] = (current_sync >> b) & 0x01;
                for (int i = 0; i < PAYLOAD_LEN; i++) for (int b = 7; b >= 0; b--) packet_bits[temp_idx++] = (tx_payload[i] >> b) & 0x01;
            }

            // BPSK-mapping + IIR-pulsforming + skriving til IQ-buffer (Q = 0).
            float target_val = packet_bits[bit_idx] ? 1.0f : -1.0f;
            current_tx_val = (current_tx_val * (1.0f - sys_alpha)) + (target_val * sys_alpha);
            ((int16_t *)p)[0] = (int16_t)(current_tx_val * sys_tx_ampl); ((int16_t *)p)[1] = 0;

            // 50 sampler per symbol; deretter neste bit (mod 544).
            if (++sample_count >= SAMPLES_PER_SYMBOL_TX) { sample_count = 0; if (++bit_idx >= 544) bit_idx = 0; }
        }
        iio_buffer_push(txbuf);                   // DMA hele bufferet til AD9363
    }
    iio_buffer_destroy(txbuf); return NULL;
}


/*
 * RX-tråd (CPU 1): leser IQ-sampler fra AD9363, desimerer ×25 og
 * kjører dem gjennom liquid-DSP-kjeden (DC-blocker, RRC-symsync,
 * Costas-PLL, BPSK-demod). Bit-strømmen drives inn i en tre-tilstands
 * maskin (st = 0 sync-søk, st = 1 kommando 24 bit, st = 2 UART 64 byte).
 */
void *rx_thread_func(void *arg) {
    (void)arg;
    struct iio_buffer *rxbuf = iio_device_create_buffer(rx_dev, RX_BUFFER_SAMPLES, false);
    if (!rxbuf) return NULL;

    // DSP-objekter fra liquid-DSP (alle bevares mellom buffer-iterasjoner).
    iirfilt_crcf dc = iirfilt_crcf_create_dc_blocker(0.01f);
    symsync_crcf ss = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, SAMPLES_PER_SYMBOL_RX, 7, 0.35f, 32);
    symsync_crcf_set_lf_bw(ss, 0.01f);
    modemcf dem = modemcf_create(LIQUID_MODEM_BPSK);
    nco_crcf pll = nco_crcf_create(LIQUID_VCO);
    nco_crcf_pll_set_bandwidth(pll, 0.05f);

    float complex in, syms[8], mix;
    unsigned int ns = 0, bit = 0;
    uint32_t sr = 0, cmd = 0;                     // sr = 32-bits rullerende skiftregister for sync-søk
    uint8_t up[PAYLOAD_LEN]; char ts[20];
    int st = 0, nbit = 0;
    bool rx_inverted = false;                     // settes ved «_INV»-treff (180° fasebytting)
    memset(up, 0, sizeof(up));

    while (keep_running) {
        if (iio_buffer_refill(rxbuf) < 0) continue;
        void *p0 = iio_buffer_first(rxbuf, rx0_i), *pe = iio_buffer_end(rxbuf);
        ptrdiff_t step = iio_buffer_step(rxbuf), jump = step * DECIMATION_FACTOR;

        for (void *p = p0; p < pe; p += jump) {
            // Skaler 12-bit ADC-verdier til komplekst flyttall i området ±1.
            float re = ((int16_t *)p)[0] / 2048.0f, im = ((int16_t *)p)[1] / 2048.0f;
            in = re + _Complex_I * im;

            iirfilt_crcf_execute(dc, in, &in);
            symsync_crcf_execute(ss, &in, 1, syms, &ns);

            // ns ∈ {0, 1, 2}: antall symboler symsync produserte for denne sample.
            for (unsigned int i = 0; i < ns; i++) {
                nco_crcf_mix_down(pll, syms[i], &mix);
                modemcf_demodulate(dem, mix, &bit);
                nco_crcf_pll_step(pll, modemcf_get_demodulator_phase_error(dem));
                nco_crcf_step(pll);

                uint8_t actual_bit = rx_inverted ? ((~bit) & 1) : (bit & 1);

                if (st == 0) {
                    // Sync-søk: skift inn rå bit (ikke invertert) og match mot fire kandidater.
                    sr = (sr << 1) | (bit & 1);
                    if (sr == COMMAND_SYNC) {
                        st = 1; nbit = 0; cmd = 0; rx_inverted = false;
                    } else if (sr == COMMAND_SYNC_INV) {
                        st = 1; nbit = 0; cmd = 0; rx_inverted = true;
                    } else if (sr == UART_SYNC) {
                        st = 2; nbit = 0; memset(up, 0, sizeof(up)); rx_inverted = false;
                    } else if (sr == UART_SYNC_INV) {
                        st = 2; nbit = 0; memset(up, 0, sizeof(up)); rx_inverted = true;
                    }
                }
                else if (st == 1) {
                    // Kommandopakke: samle 24 bit (id, val, crc).
                    cmd = (cmd << 1) | actual_bit;
                    nbit++;
                    if (nbit >= 24) {
                        uint8_t id = (cmd >> 16) & 0xFF, val = (cmd >> 8) & 0xFF, crc = cmd & 0xFF, t[2] = { id, val };
                        if (calculate_crc8(t, 2) == crc) {
                            apply_cmd(id, val); get_ts(ts, sizeof(ts));
                            printf("\n[%s] CMD ok id=%u val=%u\n", ts, id, val); fflush(stdout);
                        }
                        st = 0; nbit = 0; cmd = 0; sr = 0;
                    }
                }
                else if (st == 2) {
                    // UART-pakke: samle 512 bit (64 byte) byte for byte.
                    int bi = nbit >> 3;
                    if (bi < PAYLOAD_LEN) {
                        up[bi] = (up[bi] << 1) | actual_bit;
                        nbit++;
                    } else {
                        st = 0; nbit = 0; sr = 0; memset(up, 0, sizeof(up)); continue;
                    }
                    if (nbit >= PAYLOAD_LEN * 8) {
                        // Ferdig: dewhiten + videresend til RIU over UART.
                        int wr = rf_uart_out(up);
                        get_ts(ts, sizeof(ts));
                        uart_ack_count++;
                        if (wr > 0) {
                            printf("\n[%s] UART <= RF %dB\n", ts, wr);
                        }
                        fflush(stdout);
                        st = 0; nbit = 0; sr = 0; memset(up, 0, sizeof(up));
                    }
                }
            }
        }
    }
    iirfilt_crcf_destroy(dc); symsync_crcf_destroy(ss); modemcf_destroy(dem); nco_crcf_destroy(pll); iio_buffer_destroy(rxbuf); return NULL;
}


/*
 * main: setter opp IIO-kontekst, konfigurerer TX (868 MHz) og RX
 * (433 MHz) på AD9363, åpner UART-broen mot RIU, og starter de tre
 * trådene. TX og RX pinnes til hver sin CPU. pthread_join blokkerer
 * hovedprosessen til keep_running settes til false (av SIGINT).
 */
int main() {
    signal(SIGINT, sigint_handler);

    // ---- IIO-kontekst og PHY-enhet ----
    ctx = iio_create_local_context();
    phy_dev_global = iio_context_find_device(ctx, "ad9361-phy");

    // ---- TX-konfigurasjon: 868 MHz, 2,5 MS/s, 2 MHz båndbredde, manuell gain ----
    struct iio_channel *tx_lo = iio_device_find_channel(phy_dev_global, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev_global, "voltage0", true);
    tx_chan_global = tx_chan;
    iio_channel_attr_write_longlong(tx_lo, "frequency", 868000000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 2000000);
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 2500000);
    iio_channel_attr_write(tx_chan, "gain_control_mode", "manual");
    iio_channel_attr_write_longlong(tx_chan, "hardwaregain", sys_tx_atten);

    // ---- TX DMA-buffer-enhet ----
    tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i); iio_channel_enable(tx0_q);

    // ---- RX-konfigurasjon: 433 MHz, 2,5 MS/s, 2 MHz båndbredde, slow_attack AGC ----
    struct iio_channel *rx_lo = iio_device_find_channel(phy_dev_global, "altvoltage0", true);
    struct iio_channel *rx_chan = iio_device_find_channel(phy_dev_global, "voltage0", false);
    rx_chan_global = rx_chan;
    iio_channel_attr_write_longlong(rx_lo, "frequency", 433000000);
    iio_channel_attr_write_longlong(rx_chan, "rf_bandwidth", 2000000);
    iio_channel_attr_write_longlong(rx_chan, "sampling_frequency", 2500000);
    iio_channel_attr_write(rx_chan, "gain_control_mode", "slow_attack");

    // ---- RX DMA-buffer-enhet ----
    rx_dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    rx0_i = iio_device_find_channel(rx_dev, "voltage0", false);
    rx0_q = iio_device_find_channel(rx_dev, "voltage1", false);
    iio_channel_enable(rx0_i); iio_channel_enable(rx0_q);

    // ---- UART-bro mot RIU på ttyPS1 (115200 baud) ----
    if (uart_bridge_init("/dev/ttyPS1", 115200) != 0) { fprintf(stderr, "Kunne ikke åpne UART\\n"); }

    // ---- Start tråder med CPU-affinitet ----
    pthread_t tx_thread, rx_thread, telemetry_thread;
    cpu_set_t cpuset_tx, cpuset_rx;

    CPU_ZERO(&cpuset_tx);
    CPU_SET(0, &cpuset_tx);
    pthread_create(&tx_thread, NULL, tx_thread_func, NULL);
    pthread_setaffinity_np(tx_thread, sizeof(cpu_set_t), &cpuset_tx);

    CPU_ZERO(&cpuset_rx);
    CPU_SET(1, &cpuset_rx);
    pthread_create(&rx_thread, NULL, rx_thread_func, NULL);
    pthread_setaffinity_np(rx_thread, sizeof(cpu_set_t), &cpuset_rx);

    pthread_create(&telemetry_thread, NULL, telemetry_thread_func, NULL);   // ingen pinning

    printf("Rakett-OS Kjører! (Sikker CPU, AGC & UART Aktivert)\n");
    fflush(stdout);

    // ---- Vent til SIGINT setter keep_running = false ----
    pthread_join(tx_thread, NULL);
    pthread_join(rx_thread, NULL);
    pthread_join(telemetry_thread, NULL);

    uart_bridge_close();
    iio_context_destroy(ctx);
    return 0;
}