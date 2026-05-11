#define _GNU_SOURCE // Nødvendig for CPU Affinity
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
#include <liquid/liquid.h> // DSP-biblioteket for RX

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

// --- KONFIGURASJON ---
#define AMPLITUDE 15000
#define PAYLOAD_LEN 64
#define SAMPLES_PER_SYMBOL 5 
#define BUFFER_SAMPLES 65536 
#define SYNC_WORD 0x1ACFFC1D // NASA 32-bit Sync

// Globale pekere til IIO hardware slik at trådene kan nå dem
struct iio_context *ctx;
struct iio_device *tx_dev, *rx_dev;
struct iio_channel *tx0_i, *tx0_q, *rx0_i, *rx0_q;

// --- HJELPEFUNKSJONER ---
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
// TRÅD 1: SENDER TELEMETRI (Kjern 0)
// ==========================================
void *tx_thread_func(void *arg) {
    printf("[TX] Telemetri-tråd startet på Kjerne 0\n");
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, BUFFER_SAMPLES, false);
    
    char payload[PAYLOAD_LEN];
    uint8_t packet_bits[544]; 
    int seq_num = 1;
    float rocket_alt = 0.0;

    while (keep_running) {
        // 1. Bygg Payload
        memset(payload, 0x55, PAYLOAD_LEN);
        snprintf(payload, PAYLOAD_LEN, "ROCKET_ID: 1 | ALT: %.1fm | SEQ: %d", rocket_alt, seq_num);
        for(int i = strlen(payload); i < PAYLOAD_LEN - 1; i++) payload[i] = 0x55;
        payload[PAYLOAD_LEN - 1] = calculate_crc8((uint8_t*)payload, PAYLOAD_LEN - 1);

        // 2. Gjør om til bits
        int bit_idx = 0;
        for(int b = 31; b >= 0; b--) packet_bits[bit_idx++] = (SYNC_WORD >> b) & 0x01;
        for(int i = 0; i < PAYLOAD_LEN; i++) {
            for(int b = 7; b >= 0; b--) packet_bits[bit_idx++] = (payload[i] >> b) & 0x01;
        }

        // 3. Fyll buffer og moduler BPSK
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

        // 4. Send avgårde
        iio_buffer_push(txbuf);
        seq_num++; rocket_alt += 4.5; 
    }
    iio_buffer_destroy(txbuf);
    return NULL;
}

// ==========================================
// TRÅD 2: MOTTAR KOMMANDOER (Kjern 1)
// ==========================================
void *rx_thread_func(void *arg) {
    printf("[RX] Kommando-tråd startet på Kjerne 1\n");
    
    // Vi bruker en mye mindre buffer her for å unngå forsinkelse (latency)
    struct iio_buffer *rxbuf = iio_device_create_buffer(rx_dev, 8192, false);

    // --- Oppsett av liquid-dsp (Digital Signal Processing) ---
    agc_crcf qagc = agc_crcf_create();
    agc_crcf_set_bandwidth(qagc, 1e-3f);

    symsync_crcf qsync = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, SAMPLES_PER_SYMBOL, 7, 0.35f, 32);
    modemcf qdemod = modemcf_create(LIQUID_MODEM_BPSK);

    // --- NYTT: Costas Loop (Phase Locked Loop) ---
    nco_crcf qpll = nco_crcf_create(LIQUID_VCO);
    nco_crcf_pll_set_bandwidth(qpll, 0.05f); // Samme Loop Bandwidth som i GNU Radio (50m)

    float complex sample_in;
    float complex symbols_out[8]; // Plass til utdata fra symsync
    unsigned int num_symbols;
    unsigned int bit_out;

    uint32_t shift_reg = 0; // For å lete etter Sync Word

    while (keep_running) {
        // 1. Hent data fra luften (blokkerer til buffer er full)
        iio_buffer_refill(rxbuf);
        void *p_dat = iio_buffer_first(rxbuf, rx0_i);
        void *p_end = iio_buffer_end(rxbuf);
        ptrdiff_t p_inc = iio_buffer_step(rxbuf);

        // 2. Prosesser hver sample
        for (void *p = p_dat; p < p_end; p += p_inc) {
            // Konverter fra IIO int16 til float complex og normaliser signalet
            float i_val = ((int16_t*)p)[0] / 2048.0f;
            float q_val = ((int16_t*)p)[1] / 2048.0f;
            sample_in = i_val + _Complex_I * q_val;

            // DSP Steg 1: Automatic Gain Control
            agc_crcf_execute(qagc, sample_in, &sample_in);

            // DSP Steg 2: Symbol Sync (Finn sentrum av biten)
            symsync_crcf_execute(qsync, &sample_in, 1, symbols_out, &num_symbols);


            // DSP Steg 3: Costas Loop & Demodulering
            for (int i = 0; i < num_symbols; i++) {
                float complex symbol_in = symbols_out[i];
                float complex symbol_derotated;

                // 1. Roter signalet tilbake (Fjerner spinn på radiobølgen)
                nco_crcf_mix_down(qpll, symbol_in, &symbol_derotated);

                // 2. Gjør om til 1 eller 0
                modemcf_demodulate(qdemod, symbol_derotated, &bit_out);

                // 3. Regn ut feilen og mat den tilbake for å justere "loopen"
                float phase_error = modemcf_get_demodulator_phase_error(qdemod);
                nco_crcf_pll_step(qpll, phase_error);
                nco_crcf_step(qpll);
                
                // Mat biten inn i skiftregisteret vårt
                shift_reg = (shift_reg << 1) | bit_out;
                
                
                // NYTT: BPSK kan låse seg 180 grader opp-ned i luften.
                // Vi sjekker for både normal og invertert (opp-ned) Sync Word!
                uint32_t SYNC_INVERTED = ~SYNC_WORD; 

                if (shift_reg == SYNC_WORD || shift_reg == SYNC_INVERTED) {
                    printf("\n[RX] BINGO! Kommando oppdaget!\n");
                    if (shift_reg == SYNC_INVERTED) {
                        printf("[RX] (Fasen var opp-ned, men systemet reddet den!)\n");
                    }
                }
            }
        }
    }

    // Rydd opp RX memory
    agc_crcf_destroy(qagc);
    symsync_crcf_destroy(qsync);
    modemcf_destroy(qdemod);
    iio_buffer_destroy(rxbuf);
    nco_crcf_destroy(qpll);
    return NULL;
}

// ==========================================
// HOVEDPROGRAM (MAIN)
// ==========================================
int main() {
    signal(SIGINT, sigint_handler);
    printf("Starter Komplett Dual-Core Rakett-System...\n");

    // --- 1. SETT OPP HARDWARE (IIO) ---
    ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    
    // TX (Telemetri ned til bakken på 868 MHz)
    struct iio_channel *tx_lo = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);
    iio_channel_attr_write_longlong(tx_lo, "frequency", 868000000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 1000000); 
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 1000000);
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -10.0); 
    tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i); iio_channel_enable(tx0_q);

    // RX (Kommandoer fra bakken på 433 MHz)
    struct iio_channel *rx_lo = iio_device_find_channel(phy_dev, "altvoltage0", true);
    struct iio_channel *rx_chan = iio_device_find_channel(phy_dev, "voltage0", false);
    iio_channel_attr_write_longlong(rx_lo, "frequency", 433000000);
    iio_channel_attr_write_longlong(rx_chan, "rf_bandwidth", 1000000);
    iio_channel_attr_write_longlong(rx_chan, "sampling_frequency", 1000000);
    rx_dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    rx0_i = iio_device_find_channel(rx_dev, "voltage0", false);
    rx0_q = iio_device_find_channel(rx_dev, "voltage1", false);
    iio_channel_enable(rx0_i); iio_channel_enable(rx0_q);

    // --- 2. SETT OPP TRÅDER OG CPU-AFFINITET ---
    pthread_t tx_thread, rx_thread;
    cpu_set_t cpuset_tx, cpuset_rx;

    // Tildel TX til Kjerne 0
    CPU_ZERO(&cpuset_tx);
    CPU_SET(0, &cpuset_tx); 
    pthread_create(&tx_thread, NULL, tx_thread_func, NULL);
    pthread_setaffinity_np(tx_thread, sizeof(cpu_set_t), &cpuset_tx);

    // Tildel RX til Kjerne 1
    CPU_ZERO(&cpuset_rx);
    CPU_SET(1, &cpuset_rx);
    pthread_create(&rx_thread, NULL, rx_thread_func, NULL);
    pthread_setaffinity_np(rx_thread, sizeof(cpu_set_t), &cpuset_rx);

    printf("Systemet kjører! TX på Kjerne 0, RX på Kjerne 1. Trykk Ctrl+C for å stoppe.\n");

    // --- 3. VENT PÅ AVSLUTNING ---
    pthread_join(tx_thread, NULL);
    pthread_join(rx_thread, NULL);

    printf("Rydder opp...\n");
    iio_context_destroy(ctx);
    return 0;
}