#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <unistd.h>
#include <iio.h>

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

int main() {
    signal(SIGINT, sigint_handler);
    printf("Steg 3: Starter Filtrert BPSK (Murvegg-test)...\n");

    // =========================================================
    // 1. OPPSETT AV RADIO 
    // =========================================================
    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    iio_channel_attr_write_longlong(tx_lo, "frequency", 433000000); // Tilbake i senter
    
    // Vi setter farten ned til det laveste radioen støtter
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 525000);
    
    // MAGIEN SKJER HER: Vi lukker det fysiske filteret til 200 kHz!
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 200000); 
    
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -20.0); 

    // =========================================================
    // 2. OPPSETT AV SENDERKANALER
    // =========================================================
    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, 4000, true); 
    
    // =========================================================
    // 3. GENERER SIGNALET
    // =========================================================
    void *p_dat = iio_buffer_first(txbuf, tx0_i);
    void *p_end = iio_buffer_end(txbuf);
    ptrdiff_t p_inc = iio_buffer_step(txbuf);
    
    // 525 000 SPS / 5 samples = 105 000 bps (105 kbps data rate)
    int samples_per_symbol = 5; 
    int sample_count = 0;
    int current_bit = 1; 
    
    for (void *p = p_dat; p < p_end; p += p_inc) {
        sample_count++;
        if (sample_count >= samples_per_symbol) {
            sample_count = 0;
            current_bit = !current_bit; 
        }

        // BPSK uten faseforskyvning (I-kanalen gjør all jobben, Q er 0)
        ((int16_t*)p)[0] = current_bit ? 15000 : -15000; // I-kanal
        ((int16_t*)p)[1] = 0;                            // Q-kanal
    }

    printf("Dytter buffer til radioen...\n");
    iio_buffer_push(txbuf);

    printf("Sender filtrert BPSK på 433.000 MHz. Sjekk TinySA!\n");
    while(keep_running) { sleep(1); }

    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    return 0;
}