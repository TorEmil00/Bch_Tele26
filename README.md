# Bch_Tele26 — SDR-basert telemetrilink for Phoenix II

Bacheloroppgave ved Universitetet i Stavanger, vår 2026.
Toveis radiolink mellom suborbital-rakett og bakke-PC, basert på programvare-
definert radio (PlutoSDR med AD9363).

| Retning  | Frekvens | Innhold              |
|----------|----------|----------------------|
| Ned-link | 868 MHz  | Telemetri fra rakett |
| Opp-link | 433 MHz  | Kommandoer fra bakke |

Modulasjon: BPSK med RRC-pulsforming (α = 0.35). Pakkeformat:
SYNC + 64-byte nyttelast + CRC-8 (polynom 0x07). Se
[`docs/protokoll/pakkeformat.md`](docs/protokoll/pakkeformat.md).

## Systemkjede

```
FC ── CAN ──► RIU ── UART ──► radio-enhet ── RF (868) ──► bakke-PC
                                              ◄── RF (433) ── bakke-PC
                                                              │
                                                              └─► RIU ── (kommando) ──► FC
```

> **Status april 2026:** CAN-grensesnittet på RIU er ikke implementert ennå.
> Under test brukes en utviklings-PC som fiktiv FC, koblet til RIU over en
> seriell forbindelse som RIU videresender til radio-enheten over UART.

## Mappekart

| Mappe          | Hva                                              | Kjører på        |
|----------------|--------------------------------------------------|------------------|
| `radio-enhet/` | Hovedprogram for radio-enheten (TX/RX/telemetri) | Pluto / Zynq SoC |
| `riu-firmware/`| Pakke-bro mellom CAN og UART                     | STM32H7          |
| `gnuradio/`    | Bakke-side signalbehandling (flowgraphs)         | Bakke-PC (Linux) |
| `bakke-gui/`   | Operatørgrensesnitt                              | Bakke-PC         |
| `docs/`        | Arkitektur-tegninger og protokoll-spec           | —                |
| `scripts/`     | Oppstart, log-parsing, hjelpe-scripts            | —                |
| `tools/`       | CRC-kalkulator, pakke-simulator                  | —                |

Hver delsystem-mappe har sin egen `README.md` med bygg- og kjøreinstrukser.

## Kom i gang

```bash
git clone https://github.com/TorEmil00/Bch_Tele26.git
cd Bch_Tele26
```

Følg deretter `README.md` i mappen for det delsystemet du vil jobbe med.

## Forfattere

- Tor Emil Torgersen
- Kristian Alexander Brun
- Herman Ågotnes

Veileder: Sven Ole Aase, UiS.

## Lisens

Se [`LICENSE`](LICENSE).
