#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <iio.h>

#define AMPLITUDE 20000
#define PAYLOAD_LEN 64
#define BUFFER_SAMPLES 65500 

// 525 kSPS / 5 samples = 105 kbps data rate.
#define SAMPLES_PER_SYMBOL 5 

volatile bool keep_running = true;

void sigint_handler(int dummy) {
    printf("\n[Ctrl+C Detected] Stopping...\n");
    keep_running = false;
}

int main() {
    signal(SIGINT, sigint_handler);
    printf("Starting Hardware-Filtered Continuous BPSK Transmitter...\n");

    // =========================================================
    // 1. HARDWARE SETUP (THE MAGIC HAPPENS HERE)
    // =========================================================
    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy_dev = iio_context_find_device(ctx, "ad9361-phy");
    struct iio_channel *tx_lo   = iio_device_find_channel(phy_dev, "altvoltage1", true);
    struct iio_channel *tx_chan = iio_device_find_channel(phy_dev, "voltage0", true);

    iio_channel_attr_write_longlong(tx_lo, "frequency", 433000000);
    
    // Set sample rate to the hardware's lowest native speed
    iio_channel_attr_write_longlong(tx_chan, "sampling_frequency", 525000); 
    
    // CRITICAL FIX: Close the physical analog filter to its absolute minimum! (200 kHz)
    // This hardware brick wall will shave off the square-wave shoulders automatically.
    iio_channel_attr_write_longlong(tx_chan, "rf_bandwidth", 500000); 
    
    iio_channel_attr_write_double(tx_chan, "hardwaregain", -10.0);

    struct iio_device *tx_dev = iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc");
    struct iio_channel *tx0_i = iio_device_find_channel(tx_dev, "voltage0", true);
    struct iio_channel *tx0_q = iio_device_find_channel(tx_dev, "voltage1", true);
    iio_channel_enable(tx0_i);
    iio_channel_enable(tx0_q);

    struct iio_buffer *txbuf = iio_device_create_buffer(tx_dev, BUFFER_SAMPLES, false);

    // =========================================================
    // 2. PACKET SETUP
    // =========================================================
    uint16_t preamble = 0xAAAA; 
    uint16_t sync_word = 0xEB90; 
    
    char payload[PAYLOAD_LEN];
    memset(payload, ' ', PAYLOAD_LEN);
    
    int total_packet_bytes = 2 + 2 + PAYLOAD_LEN; 
    int total_packet_bits = total_packet_bytes * 8; 
    
    int packet_bit_idx = 0;
    int sample_count = 0;
    int seq_num = 0;
    float rocket_alt = 0.0;
    
    snprintf(payload, PAYLOAD_LEN, "ROCKET_ID: 1 | ALT: %.1fm | SEQ: %d", rocket_alt, seq_num++);
    for(int i = strlen(payload); i < PAYLOAD_LEN; i++) payload[i] = ' ';

    // =========================================================
    // 3. MAIN CONTINUOUS LOOP (Blazing Fast, Zero Math)
    // =========================================================
    printf("Transmitting raw continuous bits... Hardware will filter the rest!\n");

    while (keep_running) {
        void *p_dat = iio_buffer_first(txbuf, tx0_i);
        void *p_end = iio_buffer_end(txbuf);
        ptrdiff_t p_inc = iio_buffer_step(txbuf);

        for (void *p = p_dat; p < p_end; p += p_inc) {
            
            uint8_t current_bit = 0;
            int byte_idx = packet_bit_idx / 8;
            int bit_in_byte = 7 - (packet_bit_idx % 8); 
            
            if (byte_idx < 2) {
                current_bit = (preamble >> ( (1 - byte_idx)*8 + bit_in_byte )) & 0x01;
            } else if (byte_idx < 4) {
                current_bit = (sync_word >> ( (3 - byte_idx)*8 + bit_in_byte )) & 0x01;
            } else {
                current_bit = (payload[byte_idx - 4] >> bit_in_byte) & 0x01;
            }

            // Raw square wave output
            ((int16_t*)p)[0] = current_bit ? AMPLITUDE : -AMPLITUDE; 
            ((int16_t*)p)[1] = 0;                                    

            sample_count++;
            if (sample_count >= SAMPLES_PER_SYMBOL) {
                sample_count = 0;
                packet_bit_idx++;
                
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

    printf("Cleaning up...\n");
    iio_buffer_destroy(txbuf);
    iio_context_destroy(ctx);
    return 0;
}