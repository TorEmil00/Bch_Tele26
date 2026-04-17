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

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq;
} cpu_stat_t;

volatile float sys_alpha = 0.60f;
volatile long long sys_rx_gain = 40;  // Denne oppdateres nå live av AGC!
volatile long long sys_tx_atten = 10;
volatile int sys_tx_ampl = 15000;

volatile float sys_c0_load = 0.0f;
volatile float sys_c1_load = 0.0f;
volatile float sys_radio_temp = 0.0f;
volatile float sys_rssi = -100.0f;

volatile int cmd_ack_count = 0;
volatile int uart_ack_count = 0;
volatile int last_cmd_id = 0;
volatile int last_cmd_val = 0;

struct iio_device *phy_dev_global;
struct iio_channel *rx_chan_global;
struct iio_channel *tx_chan_global;

struct iio_context *ctx;
struct iio_device *tx_dev, *rx_dev;
struct iio_channel *tx0_i, *tx0_q, *rx0_i, *rx0_q;

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

void get_ts(char *buffer, size_t len) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    snprintf(buffer, len, "%02d:%02d:%02d", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

uint8_t calculate_crc8(uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    return crc;
}

void whiten_payload(uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) data[i] ^= (uint8_t)((i * 211 + 79) & 0xFF);
}

void apply_cmd(uint8_t id, uint8_t val) {
    if (id == 1) {
        sys_alpha = val / 255.0f;
        if (sys_alpha < 0.05f) sys_alpha = 0.05f;
    } else if (id == 2) {
        // MERK: Siden raketten nå bruker AGC, vil manuell RX-gain her sannsynligvis 
        // overskrives av AGC-motoren umiddelbart!
        sys_rx_gain = val > 75 ? 75 : val;
        if (rx_chan_global) iio_channel_attr_write_longlong(rx_chan_global, "hardwaregain", sys_rx_gain);
    } else if (id == 3) {
        sys_tx_atten = val > 80 ? 80 : val;
        if (tx_chan_global) iio_channel_attr_write_longlong(tx_chan_global, "hardwaregain", sys_tx_atten);
    } else if (id == 4) {
        sys_tx_ampl = val * 128;
    }
    last_cmd_id = id; last_cmd_val = val; cmd_ack_count++;
}

#define PAYLOAD_LEN 64
#define SAMPLES_PER_SYMBOL_TX 50
#define SAMPLES_PER_SYMBOL_RX 2
#define DECIMATION_FACTOR 25
#define TX_BUFFER_SAMPLES 65536
#define RX_BUFFER_SAMPLES 10000

#define TELEMETRY_SYNC 0x1ACFFC1D
#define COMMAND_SYNC   0x55AA55AA
#define UART_SYNC      0xA55A3CC3
// NYTT: Inverterte Sync-ord for å fange opp 180 graders fasefeil!
#define COMMAND_SYNC_INV 0xAA55AA55
#define UART_SYNC_INV    0x5AA5C33C

static int rf_uart_out(uint8_t *p) {
    whiten_payload(p, PAYLOAD_LEN);
    return uart_bridge_write_rf_payload(p);
}

void *telemetry_thread_func(void *arg) {
    (void)arg;
    while (keep_running) {
        float c0, c1;
        get_core_loads(&c0, &c1);
        sys_c0_load = c0; sys_c1_load = c1;

        long long t_val, r_val, g_val;
        struct iio_channel *tc = iio_device_find_channel(phy_dev_global, "temp0", false);
        if (tc && iio_channel_attr_read_longlong(tc, "input", &t_val) == 0) sys_radio_temp = t_val / 1000.0f;
        
        if (rx_chan_global && iio_channel_attr_read_longlong(rx_chan_global, "rssi", &r_val) == 0) sys_rssi = r_val / 100.0f;
        
        // NYTT: Leser ut gjeldende Gain fra maskinvaren (viktig når AGC styrer det!)
        if (rx_chan_global && iio_channel_attr_read_longlong(rx_chan_global, "hardwaregain", &g_val) == 0) sys_rx_gain = g_val;
        
        sleep(1);
    }
    return NULL;
}

void *tx_thread_func(void *arg) {
    (void)arg; sleep(1);
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, TX_BUFFER_SAMPLES, false);
    if (!txbuf) return NULL;

    uint8_t payload[PAYLOAD_LEN], tx_payload[PAYLOAD_LEN], packet_bits[544];
    uint32_t current_sync = TELEMETRY_SYNC;
    int bit_idx = 0, sample_count = 0; float current_tx_val = 0.0f;

    while (keep_running) {
        void *p_dat = iio_buffer_first(txbuf, tx0_i), *p_end = iio_buffer_end(txbuf);
        ptrdiff_t p_inc = iio_buffer_step(txbuf);

        for (void *p = p_dat; p < p_end; p += p_inc) {
            if (sample_count == 0 && bit_idx == 0) {
                uart_bridge_poll_input();
                memset(payload, 0, sizeof(payload)); memset(tx_payload, 0, sizeof(tx_payload));

                if (uart_bridge_get_tx_packet(payload)) {
                    current_sync = UART_SYNC; memcpy(tx_payload, payload, PAYLOAD_LEN);
                } else {
                    current_sync = TELEMETRY_SYNC;
                    float avg_cpu = (sys_c0_load + sys_c1_load) / 2.0f;
                    // NYTT: Sender med Rakettens RX Gain (G) og TX Atten (A)
                    snprintf((char *)payload, PAYLOAD_LEN,
                             "L:%d|V:%d|K:%d|UA:%d|R:%.1f|C:%02.0f|T:%.1f|G:%lld|A:%lld",
                             last_cmd_id, last_cmd_val, cmd_ack_count, uart_ack_count, 
                             sys_rssi, avg_cpu, sys_radio_temp, sys_rx_gain, sys_tx_atten);
                    payload[PAYLOAD_LEN - 1] = calculate_crc8(payload, PAYLOAD_LEN - 1);
                    memcpy(tx_payload, payload, PAYLOAD_LEN);
                }

                whiten_payload(tx_payload, PAYLOAD_LEN);
                int temp_idx = 0;
                for (int b = 31; b >= 0; b--) packet_bits[temp_idx++] = (current_sync >> b) & 0x01;
                for (int i = 0; i < PAYLOAD_LEN; i++) for (int b = 7; b >= 0; b--) packet_bits[temp_idx++] = (tx_payload[i] >> b) & 0x01;
            }

            float target_val = packet_bits[bit_idx] ? 1.0f : -1.0f;
            current_tx_val = (current_tx_val * (1.0f - sys_alpha)) + (target_val * sys_alpha);
            ((int16_t *)p)[0] = (int16_t)(current_tx_val * sys_tx_ampl); ((int16_t *)p)[1] = 0;

            if (++sample_count >= SAMPLES_PER_SYMBOL_TX) { sample_count = 0; if (++bit_idx >= 544) bit_idx = 0; }
        }
        iio_buffer_push(txbuf);
    }
    iio_buffer_destroy(txbuf); return NULL;
}

void *rx_thread_func(void *arg) {
    (void)arg;
    struct iio_buffer *rxbuf = iio_device_create_buffer(rx_dev, RX_BUFFER_SAMPLES, false);
    if (!rxbuf) return NULL;

    iirfilt_crcf dc = iirfilt_crcf_create_dc_blocker(0.01f);
    symsync_crcf ss = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, SAMPLES_PER_SYMBOL_RX, 7, 0.35f, 32);
    symsync_crcf_set_lf_bw(ss, 0.01f);
    modemcf dem = modemcf_create(LIQUID_MODEM_BPSK);
    nco_crcf pll = nco_crcf_create(LIQUID_VCO);
    nco_crcf_pll_set_bandwidth(pll, 0.05f);

    float complex in, syms[8], mix;
    unsigned int ns = 0, bit = 0; uint32_t sr = 0, cmd = 0;
    uint8_t up[PAYLOAD_LEN]; char ts[20];
    int st = 0, nbit = 0;
    bool rx_inverted = false; // NYTT: Flagg for å vite om radioen er opp-ned

    memset(up, 0, sizeof(up));
    while (keep_running) {
        if (iio_buffer_refill(rxbuf) < 0) continue;
        void *p0 = iio_buffer_first(rxbuf, rx0_i), *pe = iio_buffer_end(rxbuf);
        ptrdiff_t step = iio_buffer_step(rxbuf), jump = step * DECIMATION_FACTOR;
        
        for (void *p = p0; p < pe; p += jump) {
            float re = ((int16_t *)p)[0] / 2048.0f, im = ((int16_t *)p)[1] / 2048.0f;
            in = re + _Complex_I * im;
            iirfilt_crcf_execute(dc, in, &in);
            symsync_crcf_execute(ss, &in, 1, syms, &ns);

            for (unsigned int i = 0; i < ns; i++) {
                nco_crcf_mix_down(pll, syms[i], &mix);
                modemcf_demodulate(dem, mix, &bit);
                nco_crcf_pll_step(pll, modemcf_get_demodulator_phase_error(dem));
                nco_crcf_step(pll);

                // MAGIEN: Hvis Costas-loop låste 180 grader feil, snu biten automatisk!
                uint8_t actual_bit = rx_inverted ? ((~bit) & 1) : (bit & 1);

                if (st == 0) {
                    sr = (sr << 1) | (bit & 1); // Søker alltid med rå-bits
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
                    int bi = nbit >> 3;
                    if (bi < PAYLOAD_LEN) {
                        up[bi] = (up[bi] << 1) | actual_bit;
                        nbit++;
                    } else {
                        st = 0; nbit = 0; sr = 0; memset(up, 0, sizeof(up)); continue;
                    }
                    if (nbit >= PAYLOAD_LEN * 8) {
                        int wr = rf_uart_out(up);
                        get_ts(ts, sizeof(ts));
                        if (wr > 0) {
                            uart_ack_count++;
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

int main() {
    signal(SIGINT, sigint_handler);
    ctx = iio_create_local_context();
    phy_dev_global = iio_context_find_device(ctx, "ad9361-phy");

    struct iio_channel *tx_lo = iio_device_find_channel(phy_dev_global, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev_global, "voltage0", true);
    tx_chan_global = tx_chan;

    iio_channel_attr_write_longlong(tx_lo, "frequency", 868000000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 2000000);
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 2500000);
    iio_channel_attr_write(tx_chan, "gain_control_mode", "manual");
    iio_channel_attr_write_longlong(tx_chan, "hardwaregain", sys_tx_atten);
    tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i); iio_channel_enable(tx0_q);

    struct iio_channel *rx_lo = iio_device_find_channel(phy_dev_global, "altvoltage0", true);
    struct iio_channel *rx_chan = iio_device_find_channel(phy_dev_global, "voltage0", false);
    rx_chan_global = rx_chan;

    iio_channel_attr_write_longlong(rx_lo, "frequency", 433000000);
    iio_channel_attr_write_longlong(rx_chan, "rf_bandwidth", 2000000);
    iio_channel_attr_write_longlong(rx_chan, "sampling_frequency", 2500000);
    
    // NYTT: AGC AKTIVERT! Raketten fikser volumet sitt automatisk.
    iio_channel_attr_write(rx_chan, "gain_control_mode", "slow_attack"); 

    rx_dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    rx0_i = iio_device_find_channel(rx_dev, "voltage0", false);
    rx0_q = iio_device_find_channel(rx_dev, "voltage1", false);
    iio_channel_enable(rx0_i); iio_channel_enable(rx0_q);
    
    if (uart_bridge_init("/dev/ttyPS1", 115200) != 0) { fprintf(stderr, "Kunne ikke åpne UART\n"); }

    pthread_t tx_thread, rx_thread, telemetry_thread; cpu_set_t cpuset_tx, cpuset_rx;
    CPU_ZERO(&cpuset_tx); CPU_SET(0, &cpuset_tx);
    pthread_create(&tx_thread, NULL, tx_thread_func, NULL); pthread_setaffinity_np(tx_thread, sizeof(cpu_set_t), &cpuset_tx);

    CPU_ZERO(&cpuset_rx); CPU_SET(1, &cpuset_rx);
    pthread_create(&rx_thread, NULL, rx_thread_func, NULL); pthread_setaffinity_np(rx_thread, sizeof(cpu_set_t), &cpuset_rx);

    pthread_create(&telemetry_thread, NULL, telemetry_thread_func, NULL);
    
    printf("🚀 Rakett-OS Kjører! (AGC & Auto-Invert Phase Correction ON)\n"); fflush(stdout);
    pthread_join(tx_thread, NULL); pthread_join(rx_thread, NULL); pthread_join(telemetry_thread, NULL);
    uart_bridge_close(); iio_context_destroy(ctx); return 0;
}