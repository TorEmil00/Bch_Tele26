"""
Bakkestasjon for telemetrilenken mellom rakett og bakke-PC.

Operatorgrensesnitt med kommandolinjemeny som kommuniserer med en
GNU Radio-flytgraf via ZMQ og XMLRPC. Kjorer fire trader: hovedtrad
(CLI), rx_telemetry_thread (parsing av innkommende telemetri),
gnu_radio_diag_thread (RSSI/SNR fra flytgrafen), og
continuous_tx_thread (sender kommandoer og UART-meldinger).
"""

import zmq
import threading
import time
import struct
import queue
import random
import os
import xmlrpc.client
from datetime import datetime


##########################
# Synkord og pakkeformat #
##########################
# --------------------------------------------------------------------
# Hver pakke er 32-bits synkord + 64-byte nyttelast. Inverterte
# varianter handterer 180-graders fasetvetydighet etter Costas-laasing.
SYNC_COMMAND = [0x55, 0xAA, 0x55, 0xAA]
SYNC_TELEMETRY_BIN = "00011010110011111111110000011101"
INVERTED_SYNC_TELEMETRY_BIN = "".join(["1" if b == "0" else "0" for b in SYNC_TELEMETRY_BIN])
SYNC_UART_BIN = "10100101010110100011110011000011"
INVERTED_SYNC_UART_BIN = "".join(["1" if b == "0" else "0" for b in SYNC_UART_BIN])
PAYLOAD_BYTES = 64
PAYLOAD_BITS = PAYLOAD_BYTES * 8


#################################
# Delt tilstand mellom traadene #
#################################
# --------------------------------------------------------------------
# Skrives av rx_telemetry_thread og gnu_radio_diag_thread, leses av
# hovedtraaden i dashbord-modus. Enkeltfelts-skriving er atomaer
# under CPython sin GIL, saa ingen lock er noedvendig.
stats = {
    "last_ack": 0, "ok_packets": 0, "crc_errors": 0,
    "gnu_rssi": 0.0, "gnu_snr": 0.0,
    "rakett_temp": 0.0, "rakett_rssi": 0.0,
    "last_uart_ack": 0, "rakett_rx_gain": 0, "rakett_tx_atten": 0,
    "last_ping_rtt": 0.0,
}

# Sendekoer fylt av hovedtraaden, tomt av continuous_tx_thread.
cmd_queue = queue.Queue()
uart_single_queue = queue.Queue()

# Tilstand for kontinuerlig UART-blast.
uart_tx_seq = 0
uart_continuous_mode = False
uart_continuous_text = ""

# Hindrer at andre traader printer over dashbordet.
live_status_active = False

# Tilstand for ping/echo-RTT-maaling. ping_active settes av valg 10
# i hovedtraaden og nullstilles av rx_telemetry_thread naar svaret kommer.
ping_active = False
ping_start_time = 0.0


def get_ts():
    """Returnerer nå værende klokkeslett som HH:MM:SS-streng."""
    return datetime.now().strftime("%H:%M:%S")


def calculate_crc8(data_bytes):
    """CRC-8 med polynom 0x07 (samme som rakett-firmware bruker)."""
    crc = 0x00
    for b in data_bytes:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def rx_telemetry_thread():
    """
    Mottar bit-strømmen fra GNU Radio over ZMQ port 5555, søker etter
    fire mulige synkord, klipper 64-byte nyttelast, reverserer
    spektral utjevning, sjekker CRC, og dispatcher som telemetri eller
    UART-melding.
    """
    global ping_active, ping_start_time
    ctx = zmq.Context()
    sock = ctx.socket(zmq.SUB)
    sock.connect("tcp://127.0.0.1:5555")
    sock.setsockopt(zmq.SUBSCRIBE, b"")

    buffer = ""
    syncs = [("TEL", SYNC_TELEMETRY_BIN), ("TEL_I", INVERTED_SYNC_TELEMETRY_BIN),
             ("UART", SYNC_UART_BIN), ("UART_I", INVERTED_SYNC_UART_BIN)]

    while True:
        # Hent bits fra GNU Radio og legg dem til i buffer-strengen.
        try:
            chunk = sock.recv(zmq.NOBLOCK)
            buffer += "".join(["1" if b > 0 else "0" for b in chunk])
        except zmq.error.Again:
            time.sleep(0.001)
            if not buffer:
                continue

        # Søker etter synkord så lenge buffer har plass til en hel pakke.
        while len(buffer) >= (32 + PAYLOAD_BITS):
            # Finn tidligste forekomst av et av de fire synkordene.
            first_idx, matched_type = -1, None
            for stype, sbin in syncs:
                idx = buffer.find(sbin)
                if idx != -1:
                    if first_idx == -1 or idx < first_idx:
                        first_idx = idx
                        matched_type = stype

            if first_idx != -1:
                if len(buffer) >= (first_idx + 32 + PAYLOAD_BITS):
                    # Klipp ut nyttelasten og fjern den fra buffer.
                    payload_bin = buffer[first_idx+32 : first_idx+32+PAYLOAD_BITS]
                    buffer = buffer[first_idx+32+PAYLOAD_BITS:]

                    is_inverted, is_uart = "_I" in matched_type, "UART" in matched_type
                    if is_inverted:
                        payload_bin = "".join(["1" if b == "0" else "0" for b in payload_bin])

                    # Konverter bit-streng til byte-array og reverser hvitningen.
                    raw_bytes = [int(payload_bin[i:i+8], 2) for i in range(0, PAYLOAD_BITS, 8)]
                    p_bytes = [raw_bytes[i] ^ ((i * 211 + 79) % 256) for i in range(PAYLOAD_BYTES)]

                    if calculate_crc8(p_bytes[:63]) == p_bytes[63]:
                        if is_uart:
                            # UART-melding fra raketten: dekode tekst og print.
                            data_len = p_bytes[0]
                            if data_len <= 60:
                                uart_data = bytes(p_bytes[1:1+data_len]).decode('utf-8', errors='replace')
                                if not live_status_active:
                                    print(f"\n[{get_ts()}] 📨 [UART FRA RAKETT]: {uart_data}")
                        else:
                            # Telemetripakke: parse felter og oppdater stats.
                            stats["ok_packets"] += 1
                            text = "".join(chr(b) for b in p_bytes[:63]).replace(chr(0x55), '').replace(chr(0x20), '').replace(chr(0x00), '')
                            try:
                                parts = {k:float(v) for k,v in (item.split(':') for item in text.split('|') if ':' in item)}
                                stats["rakett_temp"] = parts.get('T', 0)
                                stats["rakett_rssi"] = parts.get('R', 0)
                                stats["rakett_rx_gain"] = parts.get('G', 0)
                                stats["rakett_tx_atten"] = parts.get('A', 0)

                                # Sjekk om kommando-ack-telleren har økt: bekreftet kommando.
                                if 'K' in parts and parts['K'] > stats["last_ack"]:
                                    stats["last_ack"] = parts['K']
                                    cmd_id = int(parts.get('L', 0))
                                    cmd_val = int(parts.get('V', 0))

                                    # Spesialhåndtering: id=99 er ping/echo, regn ut RTT.
                                    if ping_active and cmd_id == 99:
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

                                # Tilsvarende sjekk for UART-ack-telleren.
                                if 'UA' in parts and parts['UA'] > stats["last_uart_ack"]:
                                    diff = parts['UA'] - stats["last_uart_ack"]
                                    stats["last_uart_ack"] = parts['UA']
                                    if not live_status_active:
                                        print(f"\n[{get_ts()}] ✅ UART ACK: Raketten bekrefter at meldingen(e) ble skrevet til STM32!")
                            except: pass
                    else:
                        # CRC-feil: kast pakken og tell den (kun for telemetri).
                        if not is_uart: stats["crc_errors"] += 1
                else:
                    # Ikke nok bits for full pakke enda; vent på mer data.
                    break
            else:
                # Ingen synkord funnet; behold de siste 100 bit som overlapp og avslutt loekka.
                buffer = buffer[-100:] if len(buffer) > 100 else buffer
                break


def gnu_radio_diag_thread():
    """
    Lytter på RSSI (port 5557, SUB) og SNR (port 5558, PULL) fra
    GNU Radios måle takninger. Oppdaterer stats-ordboken hvert 100 ms.
    """
    ctx = zmq.Context()
    rssi_sock = ctx.socket(zmq.SUB)
    rssi_sock.connect("tcp://127.0.0.1:5557")
    rssi_sock.setsockopt(zmq.SUBSCRIBE, b"")

    snr_sock = ctx.socket(zmq.PULL)
    snr_sock.connect("tcp://127.0.0.1:5558")

    while True:
        # RSSI kommer som en float (4 byte) fra RMS+Log10-tappingen.
        try:
            r_data = rssi_sock.recv(zmq.NOBLOCK)
            if len(r_data) >= 4: stats["gnu_rssi"] = struct.unpack("f", r_data[-4:])[0]
        except zmq.error.Again: pass

        # SNR kommer som PMT-melding fra MPSK SNR Estimator Probe.
        try:
            s_data = snr_sock.recv(zmq.NOBLOCK)
            if len(s_data) >= 8:
                try:
                    import pmt
                    val = pmt.deserialize(s_data)
                    if pmt.is_number(val): stats["gnu_snr"] = pmt.to_double(val)
                except ImportError:
                    # Fallback hvis pmt ikke er installert: tolk siste 8 byte som double.
                    stats["gnu_snr"] = struct.unpack(">d", s_data[-8:])[0]
        except zmq.error.Again: pass
        time.sleep(0.1)


def build_uart_bits(text_msg):
    """
    Bygger en 64-byte UART-pakke (lengde + tekst + sekvens + CRC8),
    påføerer hvitning, og returnerer som bit-array klar for ZMQ.
    """
    global uart_tx_seq
    data_bytes = text_msg.encode('utf-8')[:60]

    # Pakke-format: [len, data..., 0, seq, crc]
    payload = [0] * 64
    payload[0] = len(data_bytes)
    for i, b in enumerate(data_bytes): payload[i+1] = b
    payload[61] = 0
    payload[62] = uart_tx_seq
    payload[63] = calculate_crc8(payload[:63])
    uart_tx_seq = (uart_tx_seq + 1) % 256

    # Spektral utjevning (samme XOR-sekvens som rakettsiden bruker).
    for i in range(64): payload[i] ^= ((i * 211 + 79) % 256)

    # Pakke med UART-synkord foran og serialiser til bits (MSB foerst).
    packet_bytes = [0xA5, 0x5A, 0x3C, 0xC3] + payload
    return bytes([(b >> i) & 1 for b in packet_bytes for i in range(7, -1, -1)])


def continuous_tx_thread():
    """
    Sender kontinuerlig bit-strenger til GNU Radio over ZMQ port 5556.
    Prioritetshierarki per iterasjon:
        cmd_queue > uart_single_queue > uart_continuous_mode > idle_random
    Hver pakke prefikses med en preamble-sekvens.
    """
    ctx = zmq.Context()
    tx_sock = ctx.socket(zmq.PUSH)
    tx_sock.setsockopt(zmq.SNDHWM, 10)
    tx_sock.bind("tcp://127.0.0.1:5556")

    # Statisk idle-fyll og preamble (genereres en gang).
    idle_random = bytes([random.choice([0, 1]) for _ in range(400)])
    preamble = bytes([1, 1, 0, 0] * 50)

    while True:
        if not cmd_queue.empty():
            # Høyeste prioritet: kommando fra menyvalg 1-4 eller 10.
            cmd_bits = cmd_queue.get()
            tx_sock.send(preamble + cmd_bits + idle_random)

        elif not uart_single_queue.empty():
            # Engangs UART-melding fra menyvalg 5.
            uart_bits = uart_single_queue.get()
            tx_sock.send(preamble + uart_bits + idle_random)
            time.sleep(0.01)

        elif uart_continuous_mode:
            # Blast-modus: bygg UART-pakke fra fast tekst og send kontinuerlig.
            uart_bits = build_uart_bits(uart_continuous_text)
            try:
                tx_sock.send(preamble + uart_bits + idle_random, zmq.NOBLOCK)
                time.sleep(0.01)
            except zmq.error.Again: time.sleep(0.01)

        else:
            # Ingenting i køene: send tilfeldige bits for å holde linken aktiv.
            try:
                tx_sock.send(idle_random, zmq.NOBLOCK)
                time.sleep(0.005)
            except zmq.error.Again: time.sleep(0.01)


def show_live_status_dashboard():
    """
    Viser sanntids-dashbord i terminalen. Tar over hovedtråden inntil
    bruker trykker ENTER (lyttes av kortvarig de-tråd).
    """
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
    """
    Starter de tre bakgrunnstrådene og kjører CLI-meny-løkka.
    Menyvalg dispatcher kommandoer/UART-meldinger til køene, eller
    utfører direkte XMLRPC-kall mot GNU Radio for gain-justering.
    """
    global uart_continuous_mode, uart_continuous_text, ping_active, ping_start_time

    # Start bakgrunnstråder (alle daemon, slik at de avsluttes med hovedprosessen).
    threading.Thread(target=rx_telemetry_thread, daemon=True).start()
    threading.Thread(target=gnu_radio_diag_thread, daemon=True).start()
    threading.Thread(target=continuous_tx_thread, daemon=True).start()
    time.sleep(1)

    while True:
        # Skriv ut menyen.
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

        # Valg 1-4: rakett-kommando som endrer et systemflagg på rakettsiden.
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

        # Valg 5: en enkelt UART-melding videresendes til STM32 via raketten.
        elif valg == '5':
            msg = input("Skriv UART-melding som skal sendes ÉN gang til STM32: ")
            uart_bits = build_uart_bits(msg)
            uart_single_queue.put(uart_bits)
            print(f"[{get_ts()}] ✉️ Melding sendt! Venter på UART-ACK fra raketten...")
            time.sleep(1.5)

        # Valg 6/7: lokalt gain-justering paa bakkestasjonen via XMLRPC.
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

        # Valg 8: åpne dashbord-modus.
        elif valg == '8': show_live_status_dashboard()

        # Valg 9: toggle kontinuerlig UART-blast.
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

        # Valg 10: ping/echo for å måle systemforsinkelse.
        elif valg == '10':
            # time.monotonic() er upaavirket av endringer i systemklokka.
            ping_start_time = time.monotonic()
            ping_active = True
            data = [99, 0]
            packet = SYNC_COMMAND + data + [calculate_crc8(data)]
            bits = [ (b >> i) & 1 for b in packet for i in range(7, -1, -1) ]
            cmd_queue.put(bytes(bits))
            print(f"[{get_ts()}] ⏱️ Sender Ping (ID 99) til raketten... Lytter etter Echo...")
            time.sleep(1.5)


if __name__ == "__main__": main()