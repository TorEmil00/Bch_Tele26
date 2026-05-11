import zmq
import threading
import time
import struct
import queue
import random
import os
import xmlrpc.client
from datetime import datetime

SYNC_COMMAND = [0x55, 0xAA, 0x55, 0xAA] 

SYNC_TELEMETRY_BIN = "00011010110011111111110000011101" 
INVERTED_SYNC_TELEMETRY_BIN = "".join(["1" if b == "0" else "0" for b in SYNC_TELEMETRY_BIN])
SYNC_UART_BIN = "10100101010110100011110011000011"
INVERTED_SYNC_UART_BIN = "".join(["1" if b == "0" else "0" for b in SYNC_UART_BIN])
PAYLOAD_BYTES = 64
PAYLOAD_BITS = PAYLOAD_BYTES * 8

stats = {
    "last_ack": 0, "ok_packets": 0, "crc_errors": 0,
    "gnu_rssi": 0.0, "gnu_snr": 0.0,
    "rakett_temp": 0.0, "rakett_rssi": 0.0,
    "last_uart_ack": 0, "rakett_rx_gain": 0, "rakett_tx_atten": 0,
    "last_ping_rtt": 0.0  # NY: Lagrer siste målte forsinkelse
}

cmd_queue = queue.Queue()
uart_single_queue = queue.Queue() # NY: Egen kø for enkelt-meldinger til STM32
uart_tx_seq = 0  
uart_continuous_mode = False
uart_continuous_text = ""
live_status_active = False

# Globale variabler for Ping/Echo
ping_active = False
ping_start_time = 0.0

def get_ts(): return datetime.now().strftime("%H:%M:%S")

def calculate_crc8(data_bytes):
    crc = 0x00
    for b in data_bytes:
        crc ^= b
        for _ in range(8): 
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc

def rx_telemetry_thread():
    global ping_active, ping_start_time
    ctx = zmq.Context()
    sock = ctx.socket(zmq.SUB)
    sock.connect("tcp://127.0.0.1:5555") 
    sock.setsockopt(zmq.SUBSCRIBE, b"")
    buffer = ""
    syncs = [ ("TEL", SYNC_TELEMETRY_BIN), ("TEL_I", INVERTED_SYNC_TELEMETRY_BIN), 
              ("UART", SYNC_UART_BIN), ("UART_I", INVERTED_SYNC_UART_BIN) ]

    while True:
        try:
            chunk = sock.recv(zmq.NOBLOCK)
            buffer += "".join(["1" if b > 0 else "0" for b in chunk])
        except zmq.error.Again:
            time.sleep(0.001)
            if not buffer:
                continue

        while len(buffer) >= (32 + PAYLOAD_BITS):
            first_idx, matched_type = -1, None
            for stype, sbin in syncs:
                idx = buffer.find(sbin)
                if idx != -1:
                    if first_idx == -1 or idx < first_idx:
                        first_idx = idx 
                        matched_type = stype

            if first_idx != -1:
                if len(buffer) >= (first_idx + 32 + PAYLOAD_BITS):
                    payload_bin = buffer[first_idx+32 : first_idx+32+PAYLOAD_BITS]
                    buffer = buffer[first_idx+32+PAYLOAD_BITS:] 
                    
                    is_inverted, is_uart = "_I" in matched_type, "UART" in matched_type
                    if is_inverted: 
                        payload_bin = "".join(["1" if b == "0" else "0" for b in payload_bin])
                    
                    raw_bytes = [int(payload_bin[i:i+8], 2) for i in range(0, PAYLOAD_BITS, 8)]
                    p_bytes = [raw_bytes[i] ^ ((i * 211 + 79) % 256) for i in range(PAYLOAD_BYTES)]
                    
                    if calculate_crc8(p_bytes[:63]) == p_bytes[63]:
                        if is_uart:
                            data_len = p_bytes[0]
                            if data_len <= 60:
                                uart_data = bytes(p_bytes[1:1+data_len]).decode('utf-8', errors='replace')
                                if not live_status_active: 
                                    print(f"\n[{get_ts()}] 📨 [UART FRA RAKETT]: {uart_data}")
                        else:
                            stats["ok_packets"] += 1
                            text = "".join(chr(b) for b in p_bytes[:63]).replace(chr(0x55), '').replace(chr(0x20), '').replace(chr(0x00), '')
                            try:
                                parts = {k:float(v) for k,v in (item.split(':') for item in text.split('|') if ':' in item)}
                                stats["rakett_temp"] = parts.get('T', 0)
                                stats["rakett_rssi"] = parts.get('R', 0)
                                stats["rakett_rx_gain"] = parts.get('G', 0) 
                                stats["rakett_tx_atten"] = parts.get('A', 0) 
                                
                                if 'K' in parts and parts['K'] > stats["last_ack"]:
                                    stats["last_ack"] = parts['K']
                                    cmd_id = int(parts.get('L', 0))
                                    cmd_val = int(parts.get('V', 0))
                                    
                                    # MAGIEN FOR PING/ECHO!
                                    # MAGIEN FOR PING/ECHO!
                                    if ping_active and cmd_id == 99:
                                        # BRUKER NÅ SYSTEM DRIFTSTID (MONOTONIC)
                                        ping_end_time = time.monotonic() 
                                        rtt = (ping_end_time - ping_start_time) * 1000
                                        stats["last_ping_rtt"] = rtt
                                        ping_active = False
                                        if not live_status_active: 
                                            print(f"\n[{get_ts()}] ⏱️ PING/ECHO MOTTATT!")
                                            print(f"    Total Round-Trip Time (RTT) : {rtt:.0f} ms")
                                            print( "    (Merk: Sende-buffer i GNU Radio skaper hoveddelen av forsinkelsen)")
                                    else:
                                        navn = ["Ukjent", "Alpha-filter", "RX Gain", "TX Atten", "TX Amplitude"]
                                        p_navn = navn[cmd_id] if 0 < cmd_id < len(navn) else str(cmd_id)
                                        if not live_status_active: 
                                            print(f"\n[{get_ts()}] 🎯 RAKETT BEKREFTER: {p_navn} ble satt til {cmd_val}!")
                                        
                                if 'UA' in parts and parts['UA'] > stats["last_uart_ack"]:
                                    diff = parts['UA'] - stats["last_uart_ack"]
                                    stats["last_uart_ack"] = parts['UA']
                                    if not live_status_active: 
                                        print(f"\n[{get_ts()}] ✅ UART ACK: Raketten bekrefter at meldingen(e) ble skrevet til STM32!")
                            except: pass 
                    else:
                        if not is_uart: stats["crc_errors"] += 1
                else:
                    break
            else:
                buffer = buffer[-100:] if len(buffer) > 100 else buffer
                break

def gnu_radio_diag_thread():
    ctx = zmq.Context()
    rssi_sock = ctx.socket(zmq.SUB)
    rssi_sock.connect("tcp://127.0.0.1:5557")
    rssi_sock.setsockopt(zmq.SUBSCRIBE, b"")
    
    snr_sock = ctx.socket(zmq.PULL)
    snr_sock.connect("tcp://127.0.0.1:5558")
    
    while True:
        try:
            r_data = rssi_sock.recv(zmq.NOBLOCK)
            if len(r_data) >= 4: stats["gnu_rssi"] = struct.unpack("f", r_data[-4:])[0]
        except zmq.error.Again: pass
        
        try:
            s_data = snr_sock.recv(zmq.NOBLOCK)
            if len(s_data) >= 8:
                try:
                    import pmt
                    val = pmt.deserialize(s_data)
                    if pmt.is_number(val): stats["gnu_snr"] = pmt.to_double(val)
                except ImportError:
                    stats["gnu_snr"] = struct.unpack(">d", s_data[-8:])[0]
        except zmq.error.Again: pass
        time.sleep(0.1)

def build_uart_bits(text_msg):
    global uart_tx_seq
    data_bytes = text_msg.encode('utf-8')[:60]
    payload = [0] * 64
    payload[0] = len(data_bytes)
    for i, b in enumerate(data_bytes): payload[i+1] = b
    payload[61] = 0
    payload[62] = uart_tx_seq
    payload[63] = calculate_crc8(payload[:63])
    uart_tx_seq = (uart_tx_seq + 1) % 256
    for i in range(64): payload[i] ^= ((i * 211 + 79) % 256)
    
    packet_bytes = [0xA5, 0x5A, 0x3C, 0xC3] + payload 
    return bytes([(b >> i) & 1 for b in packet_bytes for i in range(7, -1, -1)])

def continuous_tx_thread():
    ctx = zmq.Context()
    tx_sock = ctx.socket(zmq.PUSH)
    tx_sock.setsockopt(zmq.SNDHWM, 10) 
    tx_sock.bind("tcp://127.0.0.1:5556")
    idle_random = bytes([random.choice([0, 1]) for _ in range(400)])
    preamble = bytes([1, 1, 0, 0] * 50) 

    while True:
        if not cmd_queue.empty():
            cmd_bits = cmd_queue.get()
            tx_sock.send(preamble + cmd_bits + idle_random)
            
        elif not uart_single_queue.empty(): # NY: Sender enkelt-meldinger
            uart_bits = uart_single_queue.get()
            tx_sock.send(preamble + uart_bits + idle_random)
            time.sleep(0.01)
            
        elif uart_continuous_mode:
            uart_bits = build_uart_bits(uart_continuous_text)
            try:
                tx_sock.send(preamble + uart_bits + idle_random, zmq.NOBLOCK)
                time.sleep(0.01) 
            except zmq.error.Again: time.sleep(0.01)
        else:
            try:
                tx_sock.send(idle_random, zmq.NOBLOCK)
                time.sleep(0.005) 
            except zmq.error.Again: time.sleep(0.01) 

def show_live_status_dashboard():
    global live_status_active
    live_status_active = True
    stop_flag = False
    def wait_for_enter():
        nonlocal stop_flag
        input()
        stop_flag = True
    threading.Thread(target=wait_for_enter, daemon=True).start()

    while not stop_flag:
        os.system('cls' if os.name == 'nt' else 'clear')
        tot = stats['ok_packets'] + stats['crc_errors']
        rate = (stats['ok_packets']/tot)*100 if tot > 0 else 0
        print("="*65)
        print(f"📡 MISSION CONTROL DASHBOARD - {get_ts()}")
        print("="*65)
        print(f"📡 Bakke SNR (Signalkvalitet) : {stats['gnu_snr']:.1f} dB")
        print(f"📡 Bakke RSSI (Signalstyrke)  : {stats['gnu_rssi']:.1f} dBFS")
        print("-" * 65)
        print(f"🚀 Rakett RSSI (Signalstyrke) : {stats['rakett_rssi']:.1f} dBm")
        print(f"🚀 Rakett RX Gain (Autostyrt) : {stats['rakett_rx_gain']:.0f} dB")
        print(f"🚀 Rakett TX Attenuator       : {stats['rakett_tx_atten']:.0f} dB")
        print(f"🚀 Rakett Temperatur          : {stats['rakett_temp']:.1f} °C")
        print("-" * 65)
        print(f"📊 Link Suksessrate           : {rate:.1f}% ({stats['ok_packets']} ok)")
        print(f"✅ Mottatte UART-ACKs         : {stats['last_uart_ack']}")
        print(f"⏱️  Siste System-Ping (RTT)   : {stats['last_ping_rtt']:.1f} ms")
        print("-" * 65)
        if uart_continuous_mode: print(f"▶️  UART BLAST-MODE AKTIV    : Sender '{uart_continuous_text}'")
        else: print("🛑 UART BLAST-MODE INAKTIV  : (Av)")
        print("\n[ Trykk ENTER for å gå tilbake til hovedmenyen ]")
        time.sleep(1)
    live_status_active = False

def main():
    global uart_continuous_mode, uart_continuous_text, ping_active, ping_start_time
    threading.Thread(target=rx_telemetry_thread, daemon=True).start()
    threading.Thread(target=gnu_radio_diag_thread, daemon=True).start()
    threading.Thread(target=continuous_tx_thread, daemon=True).start()
    time.sleep(1) 
    
    while True:
        os.system('cls' if os.name == 'nt' else 'clear')
        print("\n" + "="*55)
        print("🌍 MISSION CONTROL KONTROLLPANEL")
        print("="*55)
        print("  [ RAKETT KOMMANDOER ]")
        print("  1. Endre Alpha (Filter for TX på raketten)")
        print("  2. Endre Rakettens RX Gain (Dummy)")
        print("  3. Endre Rakettens TX Demping (Atten)")
        print("  4. Endre Rakettens TX Amplitude")
        print("  5. ✉️  Send ENKELT UART-melding til STM32")
        print("\n  [ BAKKESTASJON LOKALT ]")
        print("  6. 🎛️  Endre Bakkestasjonens RX Gain")
        print("  7. 🎛️  Endre Bakkestasjonens TX Gain")
        print("\n  [ DIAGNOSTIKK & MODUS ]")
        print("  8. 📊 ÅPNE LIVE DASHBOARD")
        if uart_continuous_mode: print("  9. 🛑 STOPP KONTINUERLIG UART BLAST-MODE")
        else: print("  9. ▶️ START KONTINUERLIG UART BLAST-MODE")
        print(" 10. ⏱️  Mål Systemforsinkelse (Ping/Echo)")
            
        valg = input("\nVelg handling (1-10): ")
        
        if valg in ['1', '2', '3', '4']:
            try:
                v = int(input("Skriv inn verdi: "))
                data = [int(valg), v]
                packet = SYNC_COMMAND + data + [calculate_crc8(data)]
                bits = [ (b >> i) & 1 for b in packet for i in range(7, -1, -1) ]
                cmd_queue.put(bytes(bits))
                print(f"[{get_ts()}] >> System-kommando lagt i sending-kø...")
                time.sleep(1)
            except ValueError:
                print("Ugyldig tall!")
                time.sleep(1)
                
        elif valg == '5':
            msg = input("Skriv UART-melding som skal sendes ÉN gang til STM32: ")
            uart_bits = build_uart_bits(msg)
            uart_single_queue.put(uart_bits)
            print(f"[{get_ts()}] ✉️ Melding sendt! Venter på UART-ACK fra raketten...")
            time.sleep(1.5)
            
        elif valg == '6':
            try:
                v = float(input("Skriv inn ny RX Gain for bakkestasjonen (f.eks 40): "))
                grc = xmlrpc.client.ServerProxy("http://localhost:8080")
                grc.set_gr_rx_gain(v)
                print(f"[{get_ts()}] ✅ Bakkestasjonens RX Gain ble satt til {v} dB!")
                time.sleep(1.5)
            except Exception as e:
                print(f"\n❌ Klarte ikke koble til GNU Radio! {e}")
                time.sleep(3)
                
        elif valg == '7':
            try:
                v = float(input("Skriv inn ny TX Gain for bakkestasjonen (f.eks 10): "))
                grc = xmlrpc.client.ServerProxy("http://localhost:8080")
                grc.set_gr_tx_gain(v)
                print(f"[{get_ts()}] ✅ Bakkestasjonens TX Gain ble satt til {v} dB!")
                time.sleep(1.5)
            except Exception as e:
                print(f"\n❌ Klarte ikke koble til GNU Radio! {e}")
                time.sleep(3)

        elif valg == '8': show_live_status_dashboard()
        
        elif valg == '9':
            if uart_continuous_mode:
                uart_continuous_mode = False
                print(f"\n[{get_ts()}] 🛑 UART-sending STANSES...")
            else:
                msg = input("Skriv meldingen (Trykk Enter for 'UUUUUUUUUU'): ")
                if not msg: msg = "UUUUUUUUUU"
                uart_continuous_text = msg
                uart_continuous_mode = True
                print(f"\n[{get_ts()}] ▶️ UART BLAST-MODE AKTIVERT!")
            time.sleep(1)
            
        elif valg == '10':
            # BRUKER NÅ SYSTEM DRIFTSTID (MONOTONIC)
            ping_start_time = time.monotonic() 
            ping_active = True
            data = [99, 0] 
            packet = SYNC_COMMAND + data + [calculate_crc8(data)]
            bits = [ (b >> i) & 1 for b in packet for i in range(7, -1, -1) ]
            cmd_queue.put(bytes(bits))
            print(f"[{get_ts()}] ⏱️ Sender Ping (ID 99) til raketten... Lytter etter Echo...")
            time.sleep(1.5)

if __name__ == "__main__": main()