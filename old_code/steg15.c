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
#include <iio.h>
#include <liquid/liquid.h> 

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

// --- KONFIGURASJON ---
#define AMPLITUDE 15000
#define PAYLOAD_LEN 64
#define SAMPLES_PER_SYMBOL 5 
#define TX_BUFFER_SAMPLES 65536 
#define RX_BUFFER_SAMPLES 8192  
#define SYNC_WORD 0x1ACFFC1D 

// Globale hardware-pekere
struct iio_context *ctx;
struct iio_device *tx_dev, *rx_dev;
struct iio_channel *tx0_i, *tx0_q, *rx0_i, *rx0_q;

// ==========================================
// SELVSJEKK: Verifiserer maskinvareoppsett
// ==========================================
void verify_radio_settings(struct iio_channel *tx_lo, struct iio_channel *tx_chan, 
                           struct iio_channel *rx_lo, struct iio_channel *rx_chan) {
    long long val;
    char str_val[100];
    bool all_ok = true;

    printf("\n=== VERIFISERER HARDWARE OPPSETT ===\n");

    // --- SJEKK RX (Kommandomottaker) ---
    iio_channel_attr_read_longlong(rx_lo, "frequency", &val);
    if (llabs(val - 433000000) <= 10) printf("[OK] RX Frekvens: %lld Hz\n", val);
    else { printf("[FEIL] RX Frekvens er %lld Hz\n", val); all_ok = false; }

    iio_channel_attr_read_longlong(rx_chan, "sampling_frequency", &val);
    if (llabs(val - 1000000) <= 10) printf("[OK] RX Sample Rate: %lld Hz\n", val);
    else { printf("[FEIL] RX Sample Rate er %lld Hz\n", val); all_ok = false; }

    iio_channel_attr_read(rx_chan, "gain_control_mode", str_val, sizeof(str_val));
    str_val[strcspn(str_val, "\n")] = 0; 
    printf("[INFO] RX Gain Mode: %s\n", str_val);

    iio_channel_attr_read_longlong(rx_chan, "hardwaregain", &val);
    printf("[INFO] RX Hardware Gain: %lld dB\n", val);

    // --- SJEKK TX (Telemetrisender) ---
    iio_channel_attr_read_longlong(tx_lo, "frequency", &val);
    if (llabs(val - 868000000) <= 10) printf("[OK] TX Frekvens: %lld Hz\n", val);
    else { printf("[FEIL] TX Frekvens er %lld Hz\n", val); all_ok = false; }

    iio_channel_attr_read_longlong(tx_chan, "sampling_frequency", &val);
    if (llabs(val - 1000000) <= 10) printf("[OK] TX Sample Rate: %lld Hz\n", val);
    else { printf("[FEIL] TX Sample Rate er %lld Hz\n", val); all_ok = false; }

    printf("====================================\n");
    
    if (!all_ok) {
        printf("\n[FATAL FEIL] Hardware-oppsettet stemmer ikke! Stopper programmet for å unngå ustabilitet.\n");
        exit(1); 
    } else {
        printf("Suksess: All hardware er konfigurert riktig. Starter opp!\n\n");
    }
}

// CRC-8 for telemetri ut
uint8_t calculate_crc8(uint8_t *data, size_t len) {
    uint8_t crc = 0x00;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07;
            else crc <<= 1;
        }
    }
    return crc;
}

// ==========================================
// TRÅD 1: SENDER TELEMETRI (Kjerne 0)
// ==========================================
void *tx_thread_func(void *arg) {
    printf("[TX] Telemetri-tråd startet på Kjerne 0\n");
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, TX_BUFFER_SAMPLES, false);
    
    char payload[PAYLOAD_LEN];
    uint8_t packet_bits[544]; 
    int seq_num = 1;
    float rocket_alt = 0.0;

    while (keep_running) {
        memset(payload, 0x55, PAYLOAD_LEN);
        snprintf(payload, PAYLOAD_LEN, "ROCKET_ID: 1 | ALT: %.1fm | SEQ: %d", rocket_alt, seq_num);
        for(int i = strlen(payload); i < PAYLOAD_LEN - 1; i++) payload[i] = 0x55;
        payload[PAYLOAD_LEN - 1] = calculate_crc8((uint8_t*)payload, PAYLOAD_LEN - 1);

        int bit_idx = 0;
        for(int b = 31; b >= 0; b--) packet_bits[bit_idx++] = (SYNC_WORD >> b) & 0x01;
        for(int i = 0; i < PAYLOAD_LEN; i++) {
            for(int b = 7; b >= 0; b--) packet_bits[bit_idx++] = (payload[i] >> b) & 0x01;
        }

        void *p_dat = iio_buffer_first(txbuf, tx0_i);
        void *p_end = iio_buffer_end(txbuf);
        ptrdiff_t p_inc = iio_buffer_step(txbuf);

        bit_idx = 0; int sample_count = 0;
        for (void *p = p_dat; p < p_end; p += p_inc) {
            ((int16_t*)p)[0] = packet_bits[bit_idx] ? AMPLITUDE : -AMPLITUDE; 
            ((int16_t*)p)[1] = 0;                                    
            
            sample_count++;
            if (sample_count >= SAMPLES_PER_SYMBOL) {
                sample_count = 0; bit_idx++;
                if (bit_idx >= 544) bit_idx = 0; 
            }
        }

        iio_buffer_push(txbuf);
        seq_num++; rocket_alt += 4.5; 
    }
    iio_buffer_destroy(txbuf);
    return NULL;
}

// ==========================================
// TRÅD 2: MOTTAR KOMMANDOER (Kjerne 1)
// ==========================================
void *rx_thread_func(void *arg) {
    printf("[RX] Kommando-tråd startet på Kjerne 1\n");
    struct iio_buffer *rxbuf = iio_device_create_buffer(rx_dev, RX_BUFFER_SAMPLES, false);

    iirfilt_crcf qdc = iirfilt_crcf_create_dc_blocker(0.01f); 
    agc_crcf qagc = agc_crcf_create();
    agc_crcf_set_bandwidth(qagc, 1e-3f);
    symsync_crcf qsync = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, SAMPLES_PER_SYMBOL, 7, 0.35f, 32);
    modemcf qdemod = modemcf_create(LIQUID_MODEM_BPSK);
    nco_crcf qpll = nco_crcf_create(LIQUID_VCO);
    nco_crcf_pll_set_bandwidth(qpll, 0.05f); 

    float complex sample_in;
    float complex symbols_out[8]; 
    unsigned int num_symbols;
    unsigned int bit_out;
    uint32_t shift_reg = 0;
    uint32_t SYNC_INVERTED = ~SYNC_WORD;
    long bit_count = 0;

    while (keep_running) {
        iio_buffer_refill(rxbuf);
        void *p_dat = iio_buffer_first(rxbuf, rx0_i);
        void *p_end = iio_buffer_end(rxbuf);
        ptrdiff_t p_inc = iio_buffer_step(rxbuf);

        for (void *p = p_dat; p < p_end; p += p_inc) {
            float i_val = ((int16_t*)p)[0] / 2048.0f;
            float q_val = ((int16_t*)p)[1] / 2048.0f;
            sample_in = i_val + _Complex_I * q_val;

            iirfilt_crcf_execute(qdc, sample_in, &sample_in);
            agc_crcf_execute(qagc, sample_in, &sample_in);
            symsync_crcf_execute(qsync, &sample_in, 1, symbols_out, &num_symbols);

            for (int i = 0; i < num_symbols; i++) {
                float complex symbol_derotated;
                nco_crcf_mix_down(qpll, symbols_out[i], &symbol_derotated);
                modemcf_demodulate(qdemod, symbol_derotated, &bit_out);

                float phase_error = modemcf_get_demodulator_phase_error(qdemod);
                nco_crcf_pll_step(qpll, phase_error);
                nco_crcf_step(qpll);
                
                shift_reg = (shift_reg << 1) | bit_out;

                bit_count++;
                if (bit_count >= 200000) { 
                    printf("[RX Heartbeat] Mottar bits... Siste bit: %d | PLL Freq: %.2f\n", bit_out, nco_crcf_get_frequency(qpll));
                    bit_count = 0;
                }

                if (shift_reg == SYNC_WORD || shift_reg == SYNC_INVERTED) {
                    printf("\n[RX] BINGO! Kommando oppdaget! %s\n", (shift_reg == SYNC_INVERTED) ? "(Invertert)" : "(Normal)");
                }
            }
        }
    }

    nco_crcf_destroy(qpll);
    iirfilt_crcf_destroy(qdc); 
    agc_crcf_destroy(qagc);
    symsync_crcf_destroy(qsync);
    modemcf_destroy(qdemod);
    iio_buffer_destroy(rxbuf);
    return NULL;
}

// ==========================================
// MAIN: Hardware-init og trådstyring
// ==========================================
int main() {
    signal(SIGINT, sigint_handler);
    ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    
    // --- TX OPPSETT ---
    struct iio_channel *tx_lo = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);
    iio_channel_attr_write_longlong(tx_lo, "frequency", 868000000);
    
    // VIKTIG REKKEFØLGE: Båndbredde må settes ned FØR sample rate!
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 1000000); 
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 1000000);
    
    tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i); iio_channel_enable(tx0_q);

    // --- RX OPPSETT ---
    struct iio_channel *rx_lo = iio_device_find_channel(phy_dev, "altvoltage0", true);
    struct iio_channel *rx_chan = iio_device_find_channel(phy_dev, "voltage0", false);
    iio_channel_attr_write_longlong(rx_lo, "frequency", 433000000);
    
    // VIKTIG REKKEFØLGE: Båndbredde må settes ned FØR sample rate!
    iio_channel_attr_write_longlong(rx_chan, "rf_bandwidth", 1000000);
    iio_channel_attr_write_longlong(rx_chan, "sampling_frequency", 1000000);
    
    rx_dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    rx0_i = iio_device_find_channel(rx_dev, "voltage0", false);
    rx0_q = iio_device_find_channel(rx_dev, "voltage1", false);
    iio_channel_enable(rx0_i); iio_channel_enable(rx0_q);

    verify_radio_settings(tx_lo, tx_chan, rx_lo, rx_chan);

    pthread_t tx_thread, rx_thread;
    cpu_set_t cpuset_tx, cpuset_rx;

    CPU_ZERO(&cpuset_tx); CPU_SET(0, &cpuset_tx); 
    pthread_create(&tx_thread, NULL, tx_thread_func, NULL);
    pthread_setaffinity_np(tx_thread, sizeof(cpu_set_t), &cpuset_tx);

    CPU_ZERO(&cpuset_rx); CPU_SET(1, &cpuset_rx);
    pthread_create(&rx_thread, NULL, rx_thread_func, NULL);
    pthread_setaffinity_np(rx_thread, sizeof(cpu_set_t), &cpuset_rx);

    printf("Systemet kjører! TX Kjerne 0, RX Kjerne 1. Ctrl+C for stopp.\n");
    pthread_join(tx_thread, NULL);
    pthread_join(rx_thread, NULL);

    iio_context_destroy(ctx);
    return 0;
}