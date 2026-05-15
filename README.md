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

> **Status mai 2026:** RIU-firmware er implementert som transparent
> pakke-bro, men CAN-grensesnittet mot Flight Computer er ikke ferdig.
> Under test brukes en utviklings-PC som fiktiv FC, koblet til RIU over
> en seriell forbindelse (USB-VCP via ST-Link) som RIU videresender til
> radio-enheten over UART4.

## Repo-struktur

```
Bch_Tele26/
├── radio-enhet/            Kjører på Pluto / Zynq SoC (Linux ARM)
│   ├── src/                steg38.c, uart_bridge.c, uart_bridge.h
│   ├── include/            ad9361.h, iio.h (vendored headers)
│   ├── dist/               rakett_os (kompilert binær, deployerbar)
│   ├── scripts/            Deploy-scripts (kommer)
│   └── tests/              Test-kode (kommer)
├── riu/                    Kjører på STM32H753ZI-Nucleo (Cortex-M7)
│   └── H753_telemetri/     STM32CubeIDE-prosjekt
│       ├── Core/           main.c, stm32h7xx_it.c, syscalls.c
│       ├── Drivers/        ST HAL og CMSIS (vendored)
│       ├── H753_telemetri.ioc   CubeMX-konfigurasjon
│       └── STM32H753ZITX_*.ld   Linker-scripts
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

### RIU (STM32H753ZI-Nucleo)

Krever STM32CubeIDE (versjon 1.13 eller nyere). Hele HAL-driveren ligger
i `riu/H753_telemetri/Drivers/`, så ingen ekstern installasjon kreves.

Bygging:

```text
1. File > Open Projects from File System
2. Pek på riu/H753_telemetri/
3. Project > Build All
```

Resultat: `riu/H753_telemetri/Debug/H753_telemetri.elf` (utenfor Git).

Flash via ST-Link:

```text
Run > Run As > STM32 C/C++ Application
```

eller fra kommandolinjen:

```bash
st-flash write riu/H753_telemetri/Debug/H753_telemetri.elf 0x08000000
```

Programmet starter automatisk ved oppstart og lytter passivt på
USB-VCP og UART4 (115200 8N1).

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

## Lisens

Se [`LICENSE`](LICENSE).
