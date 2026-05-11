#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <iio.h>

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

// Konfigurasjon
#define AMPLITUDE 15000
#define PAYLOAD_LEN 64
#define SAMPLES_PER_SYMBOL 5 
#define BUFFER_SAMPLES 65536 

// Matematisk fingeravtrykk (CRC-8)
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

int main() {
    signal(SIGINT, sigint_handler);
    printf("Starter Komplett BPSK Telemetri-Sender...\n");
    printf("Bruker NASA 32-bit Sync, 4 MSPS, og CRC-8.\n");

    // 1. HARDWARE OPPRETTELSE
    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    // Innstillinger for 105 kbps BPSK (4M Sample Rate / 5 SPS)
    iio_channel_attr_write_longlong(tx_lo, "frequency", 868000000);
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 4000000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 4000000); 
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -10.0); 

    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    // Dynamisk buffer
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, BUFFER_SAMPLES, false);

    // NASA CCSDS 32-bit Sync Word
    uint32_t sync_word = 0x1ACFFC1D; 
    char payload[PAYLOAD_LEN];
    // 32 bit sync + 512 bit payload = 544 bits totalt
    uint8_t packet_bits[544]; 
    
    int seq_num = 1;
    float rocket_alt = 0.0;

    printf("Sender levende data ut i luften. Trykk Ctrl+C for å stoppe.\n");

    while (keep_running) {
        // --- BYGG PAKKEN ---
        memset(payload, 0x55, PAYLOAD_LEN);
        
        // Data (Maks 62 tegn for å ha plass til CRC)
        snprintf(payload, PAYLOAD_LEN, "ROCKET_ID: 1 | ALT: %.1fm | SEQ: %d", rocket_alt, seq_num);
        
        // Metronom-padding frem til siste byte
        for(int i = strlen(payload); i < PAYLOAD_LEN - 1; i++) {
            payload[i] = 0x55;
        }

        // Kalkuler CRC på de 63 første bytene, legg det i byte 64
        payload[PAYLOAD_LEN - 1] = calculate_crc8((uint8_t*)payload, PAYLOAD_LEN - 1);

        // --- OVERSETT TIL BITS ---
        int bit_idx = 0;
        // 32 bits Sync
        for(int b = 31; b >= 0; b--) packet_bits[bit_idx++] = (sync_word >> b) & 0x01;
        // 512 bits Data
        for(int i = 0; i < PAYLOAD_LEN; i++) {
            for(int b = 7; b >= 0; b--) packet_bits[bit_idx++] = (payload[i] >> b) & 0x01;
        }

        // --- FYLL BUFFER & MODULER BPSK ---
        void *p_dat = iio_buffer_first(txbuf, tx0_i);
        void *p_end = iio_buffer_end(txbuf);
        ptrdiff_t p_inc = iio_buffer_step(txbuf);

        bit_idx = 0;
        int sample_count = 0;

        for (void *p = p_dat; p < p_end; p += p_inc) {
            ((int16_t*)p)[0] = packet_bits[bit_idx] ? AMPLITUDE : -AMPLITUDE; 
            ((int16_t*)p)[1] = 0;                                    
            
            sample_count++;
            if (sample_count >= SAMPLES_PER_SYMBOL) {
                sample_count = 0;
                bit_idx++;
                // Gjenta pakken til bufferet er fullt
                if (bit_idx >= 544) bit_idx = 0; 
            }
        }

        // --- SEND ---
        iio_buffer_push(txbuf);
        
        // --- OPPDATER VARIABLER ---
        seq_num++;
        rocket_alt += 4.5; 
    }

    printf("\nAvslutter pent...\n");
    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    return 0;
}