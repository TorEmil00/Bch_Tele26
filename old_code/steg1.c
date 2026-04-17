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
    printf("Steg 1: Starter ren tone-sender (1 MHz offset)...\n");

    // =========================================================
    // 1. OPPSETT AV RADIO (AD9361)
    // =========================================================
    struct iio_context *ctx = iio_create_local_context();
    if (!ctx) {
        printf("Feil: Fant ikke radioen. Kjører du dette på Plutoen?\n");
        return 1;
    }
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    // Senterfrekvens på 433 MHz
    iio_channel_attr_write_longlong(tx_lo, "frequency", 433000000);
    
    // Skrur farten opp til 4 millioner samples i sekundet for å gi plass
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 4000000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 4000000);
    
    // -20 dB er en god, trygg styrke for testing på pulten
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -20.0); 

    // =========================================================
    // 2. OPPSETT AV SENDERKANALER
    // =========================================================
    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    // VIKTIG: 'true' betyr at dette er en syklisk buffer.
    // Vi fyller den én gang, og maskinvaren repeterer den i det uendelige!
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, 4096, true); 
    
    // =========================================================
    // 3. GENERER SIGNALET (MATEMATIKKEN)
    // =========================================================
    void *p_dat = iio_buffer_first(txbuf, tx0_i);
    void *p_end = iio_buffer_end(txbuf);
    ptrdiff_t p_inc = iio_buffer_step(txbuf);
    
    float phase = 0;
    for (void *p = p_dat; p < p_end; p += p_inc) {
        ((int16_t*)p)[0] = (int16_t)(cos(phase) * 15000.0); // I-kanal
        ((int16_t*)p)[1] = (int16_t)(sin(phase) * 15000.0); // Q-kanal
        
        // Med 4 MSPS vil et hopp på Pi/2 skyve signalet eksakt +1 MHz unna senter
        phase += 1.570796; 
    }

    // =========================================================
    // 4. START SENDEREN OG SOV
    // =========================================================
    printf("Dytter buffer til radioen...\n");
    iio_buffer_push(txbuf);

    printf("Sender kontinuerlig tone på 434.000 MHz. Sjekk TinySA!\n");
    printf("Trykk Ctrl+C for å stoppe.\n");
    
    // CPU-en gjør ingenting lenger. Maskinvaren pumper ut RF-en for oss.
    while(keep_running) { 
        sleep(1); 
    }

    // =========================================================
    // 5. OPPRYDNING
    // =========================================================
    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    printf("\nProgrammet ble avsluttet trygt. Radioen er av.\n");
    return 0;
}