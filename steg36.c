#define _GNU_SOURCE 
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

typedef struct { unsigned long long user, nice, system, idle, iowait, irq, softirq; } cpu_stat_t;

volatile float sys_alpha = 0.60f; 
volatile long long sys_rx_gain = 40;
volatile long long sys_tx_atten = 10; 
volatile int sys_tx_ampl = 15000; 

volatile float sys_c0_load = 0.0f;
volatile float sys_c1_load = 0.0f;
volatile float sys_radio_temp = 0.0f;
volatile float sys_rssi = -100.0f;

volatile int cmd_ack_count = 0; 
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
        if (strncmp(buffer, "cpu0 ", 5) == 0) sscanf(buffer, "cpu0 %llu %llu %llu %llu %llu %llu %llu", &curr0.user, &curr0.nice, &curr0.system, &curr0.idle, &curr0.iowait, &curr0.irq, &curr0.softirq);
        else if (strncmp(buffer, "cpu1 ", 5) == 0) sscanf(buffer, "cpu1 %llu %llu %llu %llu %llu %llu %llu", &curr1.user, &curr1.nice, &curr1.system, &curr1.idle, &curr1.iowait, &curr1.irq, &curr1.softirq);
    }
    fclose(fp);
    *load0 = calc_cpu_load(&prev0, &curr0);
    *load1 = calc_cpu_load(&prev1, &curr1);
    prev0 = curr0; prev1 = curr1;
}

void get_ts(char *buffer, size_t len) {
    struct timeval tv; gettimeofday(&tv, NULL);
    struct tm *tm_info = localtime(&tv.tv_sec);
    snprintf(buffer, len, "%02d:%02d:%02d", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
}

double get_raw_time() {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

uint8_t calculate_crc8(uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
    }
    return crc;
}

void apply_cmd(uint8_t id, uint8_t val) {
    if (id == 1) { sys_alpha = val / 255.0f; if (sys_alpha < 0.05f) sys_alpha = 0.05f; }
    else if (id == 2) { sys_rx_gain = val > 75 ? 75 : val; if (rx_chan_global) iio_channel_attr_write_longlong(rx_chan_global, "hardwaregain", sys_rx_gain); }
    else if (id == 3) { sys_tx_atten = val > 80 ? 80 : val; if (tx_chan_global) iio_channel_attr_write_longlong(tx_chan_global, "hardwaregain", sys_tx_atten); }
    else if (id == 4) { sys_tx_ampl = val * 128; }
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

void *telemetry_thread_func(void *arg) {
    while (keep_running) {
        float c0, c1; get_core_loads(&c0, &c1);
        sys_c0_load = c0; sys_c1_load = c1;
        long long t_val, r_val;
        struct iio_channel *tc = iio_device_find_channel(phy_dev_global, "temp0", false);
        if (tc && iio_channel_attr_read_longlong(tc, "input", &t_val) == 0) sys_radio_temp = t_val / 1000.0f;
        if (rx_chan_global && iio_channel_attr_read_longlong(rx_chan_global, "rssi", &r_val) == 0) sys_rssi = r_val / 100.0f;
        sleep(1); 
    }
    return NULL;
}

void *tx_thread_func(void *arg) {
    sleep(1); // ANTI-STALL
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, TX_BUFFER_SAMPLES, false);
    if (!txbuf) return NULL;
    
    char payload[PAYLOAD_LEN]; uint8_t packet_bits[544]; 
    int seq_num = 1, bit_idx = 0, sample_count = 0;
    float current_tx_val = 0.0f; 

    while (keep_running) {
        void *p_dat = iio_buffer_first(txbuf, tx0_i);
        void *p_end = iio_buffer_end(txbuf);
        ptrdiff_t p_inc = iio_buffer_step(txbuf);

        for (void *p = p_dat; p < p_end; p += p_inc) {
            if (sample_count == 0 && bit_idx == 0) {
                memset(payload, 0x20, PAYLOAD_LEN);
                snprintf(payload, PAYLOAD_LEN, "L:%d|V:%d|K:%d|R:%.1f|C0:%02.0f|C1:%02.0f|T:%.1f|U:%.3f", 
                         last_cmd_id, last_cmd_val, cmd_ack_count, sys_rssi, sys_c0_load, sys_c1_load, sys_radio_temp, get_raw_time());
                payload[PAYLOAD_LEN - 1] = calculate_crc8((uint8_t*)payload, PAYLOAD_LEN - 1);
                
                // --- DATA WHITENING (Fjerner DC Offset) ---
                for(int i = 0; i < PAYLOAD_LEN; i++) {
                    payload[i] ^= (uint8_t)((i * 211 + 79) & 0xFF);
                }
                
                int temp_idx = 0;
                for(int b = 31; b >= 0; b--) packet_bits[temp_idx++] = (TELEMETRY_SYNC >> b) & 0x01;
                for(int i = 0; i < PAYLOAD_LEN; i++) for(int b = 7; b >= 0; b--) packet_bits[temp_idx++] = (payload[i] >> b) & 0x01;
            }

            float target_val = packet_bits[bit_idx] ? 1.0f : -1.0f;
            current_tx_val = (current_tx_val * (1.0f - sys_alpha)) + (target_val * sys_alpha);

            ((int16_t*)p)[0] = (int16_t)(current_tx_val * sys_tx_ampl); 
            ((int16_t*)p)[1] = 0;                                      
            
            if (++sample_count >= SAMPLES_PER_SYMBOL_TX) { 
                sample_count = 0; 
                if (++bit_idx >= 544) bit_idx = 0; 
            }
        }
        iio_buffer_push(txbuf);
    }
    iio_buffer_destroy(txbuf);
    return NULL;
}

void *rx_thread_func(void *arg) {
    struct iio_buffer *rxbuf = iio_device_create_buffer(rx_dev, RX_BUFFER_SAMPLES, false);
    if (!rxbuf) return NULL;
    
    iirfilt_crcf qdc = iirfilt_crcf_create_dc_blocker(0.01f); 
    symsync_crcf qsync = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, SAMPLES_PER_SYMBOL_RX, 7, 0.35f, 32);
    symsync_crcf_set_lf_bw(qsync, 0.01f); 
    modemcf qdemod = modemcf_create(LIQUID_MODEM_BPSK);
    nco_crcf qpll = nco_crcf_create(LIQUID_VCO);
    nco_crcf_pll_set_bandwidth(qpll, 0.05f); 
    
    float complex sample_in, symbols_out[8]; 
    unsigned int num_symbols, bit_out;
    uint32_t shift_reg = 0;
    char ts[20];
    
    int rx_state = 0, bits_read = 0; uint32_t cmd_payload = 0; 

    while (keep_running) {
        iio_buffer_refill(rxbuf);
        void *p_dat = iio_buffer_first(rxbuf, rx0_i);
        void *p_end = iio_buffer_end(rxbuf);
        ptrdiff_t p_inc = iio_buffer_step(rxbuf);
        ptrdiff_t fast_forward = p_inc * DECIMATION_FACTOR;
        
        for (void *p = p_dat; p < p_end; p += fast_forward) {
            float real_part = ((int16_t*)p)[0] / 2048.0f;
            float imag_part = ((int16_t*)p)[1] / 2048.0f;
            if (real_part > 5.0f) real_part = 5.0f; if (real_part < -5.0f) real_part = -5.0f;
            if (imag_part > 5.0f) imag_part = 5.0f; if (imag_part < -5.0f) imag_part = -5.0f;
            
            sample_in = real_part + _Complex_I * imag_part;
            iirfilt_crcf_execute(qdc, sample_in, &sample_in);
            symsync_crcf_execute(qsync, &sample_in, 1, symbols_out, &num_symbols);
            
            for (int i = 0; i < num_symbols; i++) {
                float complex symbol_derotated;
                nco_crcf_mix_down(qpll, symbols_out[i], &symbol_derotated);
                modemcf_demodulate(qdemod, symbol_derotated, &bit_out);
                nco_crcf_pll_step(qpll, modemcf_get_demodulator_phase_error(qdemod));
                nco_crcf_step(qpll);
                
                if (rx_state == 0) {
                    shift_reg = (shift_reg << 1) | bit_out;
                    if (shift_reg == COMMAND_SYNC) { rx_state = 1; bits_read = 0; cmd_payload = 0; shift_reg = 0; }
                } else if (rx_state == 1) {
                    cmd_payload = (cmd_payload << 1) | bit_out;
                    bits_read++;
                    if (bits_read == 24) {
                        uint8_t id = (cmd_payload >> 16) & 0xFF, val = (cmd_payload >> 8) & 0xFF, crc = cmd_payload & 0xFF;
                        uint8_t chk[2] = {id, val};
                        if (calculate_crc8(chk, 2) == crc) {
                            apply_cmd(id, val);
                            get_ts(ts, sizeof(ts));
                            printf("\n[%s] 🎯 KOMMANDO VERIFISERT (ID: %d ble satt til %d)\n", ts, id, val);
                            fflush(stdout);
                        } 
                        rx_state = 0; 
                    }
                }
            }
        }
    }
    iirfilt_crcf_destroy(qdc); symsync_crcf_destroy(qsync); modemcf_destroy(qdemod); nco_crcf_destroy(qpll); iio_buffer_destroy(rxbuf);
    return NULL;
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
    iio_channel_attr_write(rx_chan, "gain_control_mode", "manual");
    iio_channel_attr_write_longlong(rx_chan, "hardwaregain", sys_rx_gain); 
    
    rx_dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    rx0_i = iio_device_find_channel(rx_dev, "voltage0", false);
    rx0_q = iio_device_find_channel(rx_dev, "voltage1", false);
    iio_channel_enable(rx0_i); iio_channel_enable(rx0_q); 

    pthread_t tx_thread, rx_thread, telemetry_thread;
    cpu_set_t cpuset_tx, cpuset_rx;
    
    CPU_ZERO(&cpuset_tx); CPU_SET(0, &cpuset_tx); 
    pthread_create(&tx_thread, NULL, tx_thread_func, NULL);
    pthread_setaffinity_np(tx_thread, sizeof(cpu_set_t), &cpuset_tx);
    
    CPU_ZERO(&cpuset_rx); CPU_SET(1, &cpuset_rx);
    pthread_create(&rx_thread, NULL, rx_thread_func, NULL);
    pthread_setaffinity_np(rx_thread, sizeof(cpu_set_t), &cpuset_rx);

    pthread_create(&telemetry_thread, NULL, telemetry_thread_func, NULL);

    printf("🚀 Rakett-OS Kjører! (Data Whitening Aktivert)\n");
    fflush(stdout);
    
    pthread_join(tx_thread, NULL); pthread_join(rx_thread, NULL); pthread_join(telemetry_thread, NULL);
    iio_context_destroy(ctx); return 0;
}