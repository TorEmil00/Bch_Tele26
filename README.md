# 🚀 PlutoSDR Flight Software

Dette prosjektet inneholder Flight Software for satellitt/rakett-kommunikasjon ved bruk av en Analog Devices PlutoSDR (Dual-Core aktivert). Programvaren mottar data fra en STM32H7 via UART og sender BPSK-modulert telemetri kontinuerlig (med RRC Pulse Shaping) for å opprettholde en stabil radiolink.

## 🛠️ Nødvendige verktøy for Cross-Compilation
For å bygge dette prosjektet for ARM-prosessoren inne i PlutoSDR-en, trenger du kompilatoren og sysroot-filene. 
*(Merk: Disse filene er store og er derfor ekskludert fra dette repoet via `.gitignore`).*

Last ned disse to filene og legg dem direkte i rotmappen av prosjektet:

1. **ARM Cross Compiler (GCC Linaro 7.3.1)**
   * **Filnavn:** `gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf.tar.xz`
   * **Last ned her:** [Linaro Releases Archive](https://releases.linaro.org/components/toolchain/binaries/7.3-2018.05/arm-linux-gnueabihf/gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf.tar.xz)

2. **PlutoSDR Sysroot (v0.39)**
   * **Filnavn:** `sysroot-v0.39.tar.gz`
   * **Last ned her:** [Analog Devices GitHub Release v0.39](https://github.com/analogdevicesinc/plutosdr-fw/releases/download/v0.39/sysroot-v0.39.tar.gz)

## 🏗️ Slik bygger du (Linux / WSL)
Etter at verktøyene er pakket ut i mappen, bruk følgende kommando for å kompilere koden for PlutoSDR-ens ARM-prosessor:

*(Her kan du senere legge inn den fulle GCC-kommandoen din når vi har satt den opp).*
