#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <iio.h>

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

int main() {
    signal(SIGINT, sigint_handler);
    printf("Steg 4: Starter ekte, tilfeldig BPSK med murvegg-filter...\n");

    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    iio_channel_attr_write_longlong(tx_lo, "frequency", 433000000); 
    
    // TVINGER farten ned til det laveste for at maskinvarefilteret skal fungere
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 525000);
    
    // LUKKER maskinvarefilteret som en murvegg for å kutte "skuldrene"
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 200000); 
    
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -20.0); 

    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    // Buffer på 4000 samples
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, 4000, true); 
    
    void *p_dat = iio_buffer_first(txbuf, tx0_i);
    void *p_end = iio_buffer_end(txbuf);
    ptrdiff_t p_inc = iio_buffer_step(txbuf);
    
    int samples_per_symbol = 5; 
    int sample_count = 0;
    int current_bit = rand() % 2; // Starter med en tilfeldig bit
    
    for (void *p = p_dat; p < p_end; p += p_inc) {
        sample_count++;
        if (sample_count >= samples_per_symbol) {
            sample_count = 0;
            // MAGIEN: Trekk en ny, tilfeldig bit (0 eller 1) for å skape ekte hvit støy-spektrum!
            current_bit = rand() % 2; 
        }

        ((int16_t*)p)[0] = current_bit ? 15000 : -15000; 
        ((int16_t*)p)[1] = 0;                            
    }

    printf("Dytter tilfeldig BPSK-buffer til radioen...\n");
    iio_buffer_push(txbuf);

    printf("Sender Ekte BPSK på 433.000 MHz. Sjekk TinySA!\n");
    while(keep_running) { sleep(1); }

    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    return 0;
}