#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <iio.h>

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

#define AMPLITUDE 15000
#define PAYLOAD_LEN 64
#define SAMPLES_PER_SYMBOL 5 
// Vi øker bufferet. Da får CPUen pusterom mens maskinvaren sender.
#define BUFFER_SAMPLES 65536 

int main() {
    signal(SIGINT, sigint_handler);
    printf("Steg 8: Starter Dynamisk Rakett-Telemetri!\n");

    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    iio_channel_attr_write_longlong(tx_lo, "frequency", 433000000);
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 4000000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 4000000); 
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -10.0); 

    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    // FALSE = Vi mater bufferet dynamisk fra C-koden
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, BUFFER_SAMPLES, false);

    uint16_t sync_word = 0xEB90; 
    char payload[PAYLOAD_LEN];
    // 16 bits sync + 512 bits payload (64 bytes * 8) = 528 bits
    uint8_t packet_bits[528]; 
    
    int seq_num = 1;
    float rocket_alt = 0.0;

    printf("Sender levende data! Sjekk Python-terminalen.\n");

    while (keep_running) {
        // 1. BYGG NY TEKST
        memset(payload, 0x55, PAYLOAD_LEN);
        snprintf(payload, PAYLOAD_LEN, "ROCKET_ID: 1 | ALT: %.1fm | SEQ: %d", rocket_alt, seq_num);
        for(int i = strlen(payload); i < PAYLOAD_LEN; i++) payload[i] = 0x55;

        // 2. OVERSETT TIL BITS
        int bit_idx = 0;
        for(int b = 15; b >= 0; b--) packet_bits[bit_idx++] = (sync_word >> b) & 0x01;
        for(int i = 0; i < PAYLOAD_LEN; i++) {
            for(int b = 7; b >= 0; b--) packet_bits[bit_idx++] = (payload[i] >> b) & 0x01;
        }

        // 3. FYLL BUFFERET LYNRASKT
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
                // Repeter samme pakke for å fylle resten av det store bufferet
                if (bit_idx >= 528) bit_idx = 0; 
            }
        }

        // 4. SEND TIL LUFTEN
        iio_buffer_push(txbuf);

        // 5. OPPDATER VARIABLER TIL NESTE RUNDE
        seq_num++;
        rocket_alt += 4.5; // Raketten stiger!
    }

    printf("\nAvslutter...\n");
    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    return 0;
}