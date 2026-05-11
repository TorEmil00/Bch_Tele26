# Bch_Tele26 — SDR-basert telemetrilink for Phoenix II

Bacheloroppgave ved Universitetet i Stavanger, vår 2026.
Toveis radiolink mellom suborbital-rakett og bakke-PC, basert på programvare-
definert radio (PlutoSDR med AD9363).

| Retning  | Frekvens | Innhold              |
|----------|----------|----------------------|
| Ned-link | 868 MHz  | Telemetri fra rakett |
| Opp-link | 433 MHz  | Kommandoer fra bakke |

Modulasjon: BPSK med RRC-pulsforming (α = 0.35) på bakke-TX, IIR-glatting
på rakett-TX. Pakkeformat: 32-bits SYNC + 64-byte nyttelast med CRC-8
(polynom 0x07). Symbolrate 50 ksymb/s ved 2,5 MS/s IQ-samplerate.

## Systemkjede

```
FC ── CAN ──► RIU ── UART ──► radio-enhet ── RF (868) ──► bakke-PC
                                              ◄── RF (433) ── bakke-PC
                                                              │
                                                              └─► RIU ── (kommando) ──► FC
```

> **Status mai 2026:** CAN-grensesnittet på RIU er ikke implementert ennå.
> Under test brukes en utviklings-PC som fiktiv FC, koblet til RIU over en
> seriell forbindelse som RIU videresender til radio-enheten over UART.

## Repo-struktur

```
Bch_Tele26/
├── radio-enhet/            Kjører på Pluto / Zynq SoC (Linux ARM)
│   ├── src/                steg38.c, uart_bridge.c, uart_bridge.h
│   ├── include/            ad9361.h, iio.h (vendored headers)
│   ├── dist/               rakett_os (kompilert binær, deployerbar)
│   ├── scripts/            Deploy-scripts (kommer)
│   └── tests/              Test-kode (kommer)
├── bakke-gui/              Kjører på bakke-PC (Python)
│   └── Bakkestasjon.py     Operatorgrensesnitt med live dashboard
├── gnuradio/               Kjører på bakke-PC (GNU Radio)
│   └── flowgraphs/         BPSK_Ground_full_system.grc
├── archive/
│   └── old_code/           Eldre stegversjoner (steg1.c – steg17.c)
├── docs/                   Arkitektur-tegninger, protokoll-spec
├── README.md
├── LICENSE                 MIT
└── .gitignore
```

## Bygging og kjøring

### Radio-enheten (Pluto / Zynq SoC)

Krever cross-compile-toolchain `gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf`
og Pluto-SDK med sysroot. Ikke i repoet — lastes ned fra Analog Devices.

Typisk byggekommando fra utviklings-PC (lokalt mappenavn `pluto_libs/`,
`liquid-dsp/` ligger ved siden av repoet i utviklings-miljøet):

```bash
arm-linux-gnueabihf-gcc -O3 -march=armv7-a -mfloat-abi=hard -mfpu=neon \
    radio-enhet/src/steg38.c radio-enhet/src/uart_bridge.c \
    -o radio-enhet/dist/rakett_os \
    -I radio-enhet/include -I. -L./pluto_libs \
    -Wl,-rpath-link=./pluto_libs -Wl,--allow-shlib-undefined \
    -liio -lad9361 -lpthread ./liquid-dsp/libliquid.a -lm
```

Deploy til Pluto:
```bash
scp radio-enhet/dist/rakett_os root@pluto:/mnt/jffs2/rakett_os
```

### Bakke-side

GNU Radio:
```bash
gnuradio-companion gnuradio/flowgraphs/BPSK_Ground_full_system.grc
```

Operatorgrensesnitt:
```bash
python3 bakke-gui/Bakkestasjon.py
```

## Forfattere

- Tor Emil Torgersen
- Kristian Alexander Brun
- Herman Ågotnes

Veileder: Sven Ole Aase, UiS.

## Lisens

Se [`LICENSE`](LICENSE).