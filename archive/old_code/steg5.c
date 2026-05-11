#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <iio.h>

#define AMPLITUDE 15000
#define PAYLOAD_LEN 64
#define SAMPLES_PER_SYMBOL 5 
#define BUFFER_SAMPLES 32768 

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

int main() {
    signal(SIGINT, sigint_handler);
    printf("Steg 5: Starter endelig telemetri-sender...\n");

    // =========================================================
    // 1. HARDWARE (De perfekte innstillingene vi fant i Steg 4)
    // =========================================================
    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    iio_channel_attr_write_longlong(tx_lo, "frequency", 433000000);
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 525000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 200000);
    
    // Skrur opp volumet til -10 dB for GNU Radio mottakeren!
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -10.0); 

    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    // Her bruker vi "false" på slutten. Vi må fylle bufferet manuelt hver gang
    // fordi raketthøyden (dataene) endrer seg hele tiden.
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, BUFFER_SAMPLES, false);

    // =========================================================
    // 2. PAKKE-OPPSETT
    // =========================================================
    uint16_t preamble = 0xAAAA; 
    uint16_t sync_word = 0xEB90; 
    
    char payload[PAYLOAD_LEN];
    int total_packet_bits = (2 + 2 + PAYLOAD_LEN) * 8; // 68 bytes * 8 = 544 bits
    
    int packet_bit_idx = 0;
    int sample_count = 0;
    int seq_num = 0;
    float rocket_alt = 0.0;
    
    // Bygger den første pakken
    memset(payload, 0x55, PAYLOAD_LEN); // Fyll med '01010101' metronom-beats
    snprintf(payload, PAYLOAD_LEN, "ROCKET_ID: 1 | ALT: %.1fm | SEQ: %d", rocket_alt, seq_num++);
    // Fyller resten av pakken med metronomen for å holde mottaker-klokken stabil
    for(int i = strlen(payload); i < PAYLOAD_LEN; i++) payload[i] = 0x55;

    // =========================================================
    // 3. HOVEDLØKKEN
    // =========================================================
    printf("Sender live telemetri. Sjekk at TinySA fortsatt ser ut som en blokk!\n");

    while (keep_running) {
        void *p_dat = iio_buffer_first(txbuf, tx0_i);
        void *p_end = iio_buffer_end(txbuf);
        ptrdiff_t p_inc = iio_buffer_step(txbuf);

        for (void *p = p_dat; p < p_end; p += p_inc) {
            
            // Hvilken bit skal sendes akkurat nå?
            uint8_t current_bit = 0;
            int byte_idx = packet_bit_idx / 8;
            int bit_in_byte = 7 - (packet_bit_idx % 8); 
            
            if (byte_idx < 2) current_bit = (preamble >> ( (1 - byte_idx)*8 + bit_in_byte )) & 0x01;
            else if (byte_idx < 4) current_bit = (sync_word >> ( (3 - byte_idx)*8 + bit_in_byte )) & 0x01;
            else current_bit = (payload[byte_idx - 4] >> bit_in_byte) & 0x01;

            // BPSK modulasjon
            ((int16_t*)p)[0] = current_bit ? AMPLITUDE : -AMPLITUDE; 
            ((int16_t*)p)[1] = 0;                                    

            // Tidsstyring (5 samples per bit)
            sample_count++;
            if (sample_count >= SAMPLES_PER_SYMBOL) {
                sample_count = 0;
                packet_bit_idx++;
                
                // Er pakken ferdig? Bygg neste pakke!
                if (packet_bit_idx >= total_packet_bits) {
                    packet_bit_idx = 0;
                    rocket_alt += 5.5;
                    snprintf(payload, PAYLOAD_LEN, "ROCKET_ID: 1 | ALT: %.1fm | SEQ: %d", rocket_alt, seq_num++);
                    for(int i = strlen(payload); i < PAYLOAD_LEN; i++) payload[i] = 0x55;
                }
            }
        }
        
        iio_buffer_push(txbuf);
    }

    printf("\nAvslutter...\n");
    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    return 0;
}