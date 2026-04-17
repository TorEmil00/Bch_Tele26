#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <iio.h>

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

#define AMPLITUDE 15000
#define PAYLOAD_LEN 64
#define SAMPLES_PER_SYMBOL 5 
// 544 bits * 5 samples = Nøyaktig 2720 samples! En perfekt, sømløs loop.
#define BUFFER_SAMPLES 2720 

int main() {
    signal(SIGINT, sigint_handler);
    printf("Steg 7: Den Evige Pakken (Syklisk Sender)...\n");

    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    // Kjører hardt og raskt, slik GNU Radio forventer
    iio_channel_attr_write_longlong(tx_lo, "frequency", 433000000);
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 4000000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 4000000); 
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -10.0); 

    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    // MAGIEN ER TILBAKE: "true" = Maskinvaren repeterer dette for alltid uten pauser.
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, BUFFER_SAMPLES, true);

    // 1. Bygg en statisk testpakke
    uint8_t packet_bits[544];
    uint16_t preamble = 0xAAAA; 
    uint16_t sync_word = 0xEB90; 
    char payload[PAYLOAD_LEN];
    
    memset(payload, 0x55, PAYLOAD_LEN);
    // Vi setter inn en fast test-tekst
    snprintf(payload, PAYLOAD_LEN, "ROCKET_ID: 1 | ALT: 100.0m | TEST OK!");
    for(int i = strlen(payload); i < PAYLOAD_LEN; i++) payload[i] = 0x55;

    // 2. Gjør alt om til en array med 1-ere og 0-ere
    int bit_idx = 0;
    for(int b = 15; b >= 0; b--) packet_bits[bit_idx++] = (preamble >> b) & 0x01;
    for(int b = 15; b >= 0; b--) packet_bits[bit_idx++] = (sync_word >> b) & 0x01;
    for(int i = 0; i < PAYLOAD_LEN; i++) {
        for(int b = 7; b >= 0; b--) packet_bits[bit_idx++] = (payload[i] >> b) & 0x01;
    }

    // 3. Fyll det sykliske bufferet
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
        }
    }

    // 4. Send og la CPU sove
    printf("Dytter syklisk buffer til radioen...\n");
    iio_buffer_push(txbuf);

    printf("Sender et matematisk feilfritt, kontinuerlig signal. Sjekk GNU Radio!\n");
    while(keep_running) { sleep(1); }

    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    return 0;
}