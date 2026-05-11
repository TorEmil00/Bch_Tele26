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
    printf("Steg 2: Starter BPSK-sender (101010... testmønster)...\n");

    // =========================================================
    // 1. OPPSETT AV RADIO 
    // =========================================================
    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    iio_channel_attr_write_longlong(tx_lo, "frequency", 433000000);
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 4000000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 4000000);
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -20.0); 

    // =========================================================
    // 2. OPPSETT AV SENDERKANALER
    // =========================================================
    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    // VIKTIG: Bufferstørrelsen er nå 4000.
    // 4000 er perfekt delelig på 40 samples, så vi kapper ikke en bit i to når bufferet repeteres!
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, 4000, true); 
    
    // =========================================================
    // 3. GENERER BPSK-SIGNALET (10101010...)
    // =========================================================
    void *p_dat = iio_buffer_first(txbuf, tx0_i);
    void *p_end = iio_buffer_end(txbuf);
    ptrdiff_t p_inc = iio_buffer_step(txbuf);
    
    // 4 000 000 samples per sek / 40 samples = 100 000 bits per sek (100 kbps)
    int samples_per_symbol = 40; 
    int sample_count = 0;
    int current_bit = 1; // Vi starter med å sende en '1'
    
    float phase = 0;
    for (void *p = p_dat; p < p_end; p += p_inc) {
        
        // Sjekk om det er på tide å bytte til neste bit
        sample_count++;
        if (sample_count >= samples_per_symbol) {
            sample_count = 0;
            current_bit = !current_bit; // Bytt fra 1 til 0, eller 0 til 1
        }

        // BPSK-magien: Hvis bit er 1, gang med +1.0. Hvis bit er 0, gang med -1.0.
        float symbol_amplitude = current_bit ? 1.0f : -1.0f;

        ((int16_t*)p)[0] = (int16_t)(symbol_amplitude * cos(phase) * 15000.0);
        ((int16_t*)p)[1] = (int16_t)(symbol_amplitude * sin(phase) * 15000.0);
        
        phase += 1.570796; // Dytter signalet +1 MHz til høyre (434 MHz)
    }

    // =========================================================
    // 4. START SENDEREN OG SOV
    // =========================================================
    printf("Dytter BPSK-buffer til radioen...\n");
    iio_buffer_push(txbuf);

    printf("Sender kontinuerlig data (100 kbps) på 434.000 MHz.\n");
    printf("Sjekk TinySA! Du skal nå se en bred 'bakke' rundt 434 MHz. Trykk Ctrl+C for å stoppe.\n");
    
    while(keep_running) { sleep(1); }

    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    printf("\nProgrammet ble avsluttet trygt.\n");
    return 0;
}