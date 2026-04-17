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
#include <iio.h>
#include <liquid/liquid.h> 

// --- SYSTEM KONSTANTER ---
#define PAYLOAD_LEN 64
#define SAMPLES_PER_SYMBOL_TX 50  
#define SAMPLES_PER_SYMBOL_RX 2    
#define DECIMATION_FACTOR 25       
#define TX_BUFFER_SAMPLES 65536 
#define RX_BUFFER_SAMPLES 10000  
#define TELEMETRY_SYNC 0x1ACFFC1D 
#define COMMAND_SYNC   0x55AA55AA

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

typedef struct { unsigned long long user, nice, system, idle, iowait, irq, softirq; } cpu_stat_t;

// --- DYNAMISKE SYSTEMVARIABLER (Fjernstyres) ---
volatile float sys_alpha = 0.60f; 
volatile long long sys_rx_gain = 40;
volatile long long sys_tx_atten = 10; 
volatile int sys_tx_ampl = 15000;      

volatile float sys_c0=0, sys_c1=0, sys_temp=0, sys_rssi=-100;
volatile int cmd_ack = 0, last_id = 0, last_val = 0;

struct iio_context *ctx;
struct iio_device *phy, *tx_d, *rx_d;
struct iio_channel *rx_ph, *tx_ph, *tx0_i, *rx0_i;

// --- HJELPEFUNKSJONER ---
uint8_t crc8(uint8_t *d, size_t len) {
    uint8_t c = 0;
    for (size_t i=0; i<len; i++) {
        c ^= d[i];
        for (int j=0; j<8; j++) c = (c & 0x80) ? (c << 1) ^ 0x07 : (c << 1);
    }
    return c;
}

void get_core_loads(float *l0, float *l1) {
    static unsigned long long p_u0=0, p_i0=0, p_u1=0, p_i1=0;
    unsigned long long u0, n0, s0, i0, u1, n1, s1, i1;
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;
    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        if (strncmp(buf, "cpu0 ", 5) == 0) sscanf(buf, "cpu0 %llu %llu %llu %llu", &u0, &n0, &s0, &i0);
        if (strncmp(buf, "cpu1 ", 5) == 0) sscanf(buf, "cpu1 %llu %llu %llu %llu", &u1, &n1, &s1, &i1);
    }
    fclose(fp);
    unsigned long long t0=u0+n0+s0+i0, t1=u1+n1+s1+i1;
    if (t0-p_u0-p_i0 != 0) *l0 = (1.0f - (float)(i0-p_i0)/(t0-p_u0-p_i0)) * 100.0f;
    if (t1-p_u1-p_i1 != 0) *l1 = (1.0f - (float)(i1-p_i1)/(t1-p_u1-p_i1)) * 100.0f;
    p_u0=t0-i0; p_i0=i0; p_u1=t1-i1; p_i1=i1;
}

double get_ts() {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

// --- BAKGRUNNSTRÅD (Telemetri) ---
void *telemetry_thread(void *arg) {
    while (keep_running) {
        get_core_loads((float*)&sys_c0, (float*)&sys_c1);
        long long r, t;
        if (rx_ph && iio_channel_attr_read_longlong(rx_ph, "rssi", &r) == 0) sys_rssi = r/100.0f;
        struct iio_channel *tc = iio_device_find_channel(phy, "temp0", false);
        if (tc && iio_channel_attr_read_longlong(tc, "input", &t) == 0) sys_temp = t/1000.0f;
        usleep(500000);
    }
    return NULL;
}

// --- UTFØR KOMMANDO FRA BAKKEN ---
void apply_cmd(uint8_t id, uint8_t val) {
    if (id==1) { sys_alpha = val/255.0f; if(sys_alpha<0.05) sys_alpha=0.05; }
    else if (id==2) { sys_rx_gain = val>75?75:val; iio_channel_attr_write_longlong(rx_ph, "hardwaregain", sys_rx_gain); }
    else if (id==3) { sys_tx_atten = val>80?80:val; iio_channel_attr_write_longlong(tx_ph, "hardwaregain", sys_tx_atten); }
    else if (id==4) { sys_tx_ampl = val * 128; }
    last_id = id; last_val = val; cmd_ack++;
}

// --- TX TRÅD (Kjerne 0) ---
void *tx_thread(void *arg) {
    struct iio_buffer *b = iio_device_create_buffer(tx_d, TX_BUFFER_SAMPLES, false);
    if (!b) { printf("[FEIL] Kunne ikke opprette TX Buffer (Segfault avverget)!\n"); keep_running = false; return NULL; }
    
    char pld[PAYLOAD_LEN]; uint8_t bits[544];
    int b_idx=0, s_cnt=0; float cur_v=0;

    while (keep_running) {
        void *p_dat = iio_buffer_first(b, tx0_i);
        for (void *p = p_dat; p < iio_buffer_end(b); p += iio_buffer_step(b)) {
            if (s_cnt == 0 && b_idx == 0) {
                memset(pld, 0x20, PAYLOAD_LEN);
                snprintf(pld, PAYLOAD_LEN, "L:%d|V:%d|K:%d|R:%.1f|T:%.1f|C0:%.0f|C1:%.0f|U:%.3f", 
                         last_id, last_val, cmd_ack, sys_rssi, sys_temp, sys_c0, sys_c1, get_ts());
                pld[PAYLOAD_LEN-1] = crc8((uint8_t*)pld, PAYLOAD_LEN-1);
                
                for(int i=0; i<32; i++) bits[i] = (TELEMETRY_SYNC >> (31-i)) & 1;
                for(int i=0; i<PAYLOAD_LEN; i++) for(int j=7; j>=0; j--) bits[32+i*8+(7-j)] = (pld[i] >> j) & 1;
            }
            float trg = bits[b_idx] ? 1.0f : -1.0f;
            cur_v = (cur_v * (1.0f - sys_alpha)) + (trg * sys_alpha);
            ((int16_t*)p)[0] = (int16_t)(cur_v * sys_tx_ampl);
            ((int16_t*)p)[1] = 0;
            if (++s_cnt >= SAMPLES_PER_SYMBOL_TX) { s_cnt=0; if (++b_idx >= 544) b_idx=0; }
        }
        iio_buffer_push(b);
    }
    iio_buffer_destroy(b);
    return NULL;
}

// --- RX TRÅD (Kjerne 1) ---
void *rx_thread(void *arg) {
    struct iio_buffer *b = iio_device_create_buffer(rx_d, RX_BUFFER_SAMPLES, false);
    if (!b) { printf("[FEIL] Kunne ikke opprette RX Buffer (Segfault avverget)!\n"); keep_running = false; return NULL; }

    symsync_crcf s = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, SAMPLES_PER_SYMBOL_RX, 7, 0.35f, 32);
    modemcf m = modemcf_create(LIQUID_MODEM_BPSK);
    nco_crcf n = nco_crcf_create(LIQUID_VCO);
    uint32_t sr=0; int state=0, b_rd=0; uint32_t pld=0;

    while (keep_running) {
        iio_buffer_refill(b);
        void *p_dat = iio_buffer_first(b, rx0_i);
        for (void *p = p_dat; p < iio_buffer_end(b); p += iio_buffer_step(b)*DECIMATION_FACTOR) {
            float complex smp = (((int16_t*)p)[0]/2048.0f) + _Complex_I * (((int16_t*)p)[1]/2048.0f);
            float complex sym; unsigned int nw;
            symsync_crcf_execute(s, &smp, 1, &sym, &nw);
            for(int i=0; i<nw; i++) {
                unsigned int bit; float complex drt;
                nco_crcf_mix_down(n, sym, &drt);
                modemcf_demodulate(m, drt, &bit);
                nco_crcf_pll_step(n, modemcf_get_demodulator_phase_error(m)); nco_crcf_step(n);
                
                if (state == 0) {
                    sr = (sr << 1) | bit;
                    if (sr == COMMAND_SYNC) { state=1; b_rd=0; pld=0; sr=0; }
                } else {
                    pld = (pld << 1) | bit;
                    if (++b_rd == 24) {
                        uint8_t id = (pld >> 16) & 0xFF, v = (pld >> 8) & 0xFF, c = pld & 0xFF;
                        uint8_t chk[2] = {id, v};
                        if (crc8(chk, 2) == c) apply_cmd(id, v);
                        state = 0;
                    }
                }
            }
        }
    }
    symsync_crcf_destroy(s); modemcf_destroy(m); nco_crcf_destroy(n); iio_buffer_destroy(b);
    return NULL;
}

int main() {
    signal(SIGINT, sigint_handler);
    printf("Initialiserer maskinvare...\n");
    ctx = iio_create_local_context();
    if (!ctx) { printf("[FEIL] Kunne ikke koble til PlutoSDR. Sjekk strøm/root.\n"); return -1; }
    
    phy = iio_context_find_device(ctx, "ad9361-phy");
    if (!phy) { printf("[FEIL] Fant ikke ad9361-phy.\n"); return -1; }

    // --- Konfigurer TX (868 MHz) ---
    tx_ph = iio_device_find_channel(phy, "voltage0", true);
    iio_channel_attr_write_longlong(iio_device_find_channel(phy, "altvoltage1", true), "frequency", 868000000);
    iio_channel_attr_write_longlong(tx_ph, "rf_bandwidth", 2000000);
    iio_channel_attr_write_longlong(tx_ph, "sampling_frequency", 2500000);
    iio_channel_attr_write(tx_ph, "gain_control_mode", "manual");
    iio_channel_attr_write_longlong(tx_ph, "hardwaregain", sys_tx_atten);

    // --- Konfigurer RX (433 MHz) ---
    rx_ph = iio_device_find_channel(phy, "voltage0", false);
    iio_channel_attr_write_longlong(iio_device_find_channel(phy, "altvoltage0", true), "frequency", 433000000);
    iio_channel_attr_write_longlong(rx_ph, "rf_bandwidth", 2000000);
    iio_channel_attr_write_longlong(rx_ph, "sampling_frequency", 2500000);
    iio_channel_attr_write(rx_ph, "gain_control_mode", "manual");
    iio_channel_attr_write_longlong(rx_ph, "hardwaregain", sys_rx_gain);

    tx_d = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    rx_d = iio_context_find_device(ctx, "cf-ad9361-lpc");
    tx0_i = iio_device_find_channel(tx_d, "voltage0", true);
    rx0_i = iio_device_find_channel(rx_d, "voltage0", false);
    iio_channel_enable(tx0_i); iio_channel_enable(iio_device_find_channel(tx_d, "voltage1", true));
    iio_channel_enable(rx0_i); iio_channel_enable(iio_device_find_channel(rx_d, "voltage1", false));

    pthread_t t1, t2, t3; cpu_set_t cs;
    CPU_ZERO(&cs); CPU_SET(0, &cs); pthread_create(&t1, NULL, tx_thread, NULL); pthread_setaffinity_np(t1, sizeof(cs), &cs);
    CPU_ZERO(&cs); CPU_SET(1, &cs); pthread_create(&t2, NULL, rx_thread, NULL); pthread_setaffinity_np(t2, sizeof(cs), &cs);
    pthread_create(&t3, NULL, telemetry_thread, NULL);

    printf("🚀 RAKETT-OS Klar for Mission Control!\n");
    pthread_join(t1, NULL); pthread_join(t2, NULL); pthread_join(t3, NULL);
    iio_context_destroy(ctx);
    printf("Avsluttet.\n"); return 0;
}