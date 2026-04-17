#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <iio.h>

volatile bool keep_running = true;
void sigint_handler(int dummy) { keep_running = false; }

int main() {
    signal(SIGINT, sigint_handler);
    printf("Starting Golden Hardware Test...\n");

    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    iio_channel_attr_write_longlong(tx_lo, "frequency", 433000000);
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 1000000);
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 1000000);
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -20.0);

    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    // CRITICAL FIX: The 'true' at the end makes this a CYCLIC buffer.
    // The hardware will repeat this buffer infinitely with ZERO gaps.
    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, 4096, true); 
    
    // Generate a pure sine wave test tone
    void *p_dat = iio_buffer_first(txbuf, tx0_i);
    void *p_end = iio_buffer_end(txbuf);
    ptrdiff_t p_inc = iio_buffer_step(txbuf);
    
    float phase = 0;
    for (void *p = p_dat; p < p_end; p += p_inc) {
        ((int16_t*)p)[0] = (int16_t)(cos(phase) * 20000.0); // I
        ((int16_t*)p)[1] = (int16_t)(sin(phase) * 20000.0); // Q
        phase += 0.1; // Frequency of the tone
    }

    // Push it EXACTLY ONCE.
    printf("Pushing cyclic buffer to hardware...\n");
    iio_buffer_push(txbuf);

    printf("Hardware is transmitting perfectly continuously. Check TinySA. Press Ctrl+C to stop.\n");
    
    // Put the CPU completely to sleep. The hardware is doing 100% of the work now.
    while(keep_running) {
        sleep(1);
    }

    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    printf("Test finished.\n");
    return 0;
}