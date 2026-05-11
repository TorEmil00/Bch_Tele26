#!/bin/bash
# ============================================================================
#  Bch_Tele26 — restruktureringsscript (2026-05-11)
#
#  Kjor i WSL fra repo-roten:
#      cd "/mnt/c/.../Bch_Tele26- rakket del_linux wsl"
#      chmod +x restruktur.sh
#      ./restruktur.sh
# ============================================================================

set -uo pipefail
cd "$(dirname "$0")"

echo "== 1/7  Klargjor branch =="
git checkout main
if git rev-parse --verify --quiet restruktur >/dev/null 2>&1; then
    echo "  Sletter gammel restruktur-branch"
    git branch -D restruktur
fi
git checkout -b restruktur

echo ""
echo "== 2/7  Lager mappestruktur =="
mkdir -p radio-enhet/src radio-enhet/include radio-enhet/scripts radio-enhet/tests radio-enhet/dist
mkdir -p bakke-gui
mkdir -p gnuradio/flowgraphs
mkdir -p docs
mkdir -p archive

echo ""
echo "== 3/7  Flytter kildekode med git mv =="

mv_if_exists() {
    if [ -e "$1" ]; then
        git mv "$1" "$2"
        echo "  $1  ->  $2"
    else
        echo "  hopper over $1 (finnes ikke)"
    fi
}

# Rakett-firmware (gjeldende)
mv_if_exists 'steg38.c'                      'radio-enhet/src/steg38.c'
mv_if_exists 'uart_bridge.c'                 'radio-enhet/src/uart_bridge.c'
mv_if_exists 'uart_bridge.h'                 'radio-enhet/src/uart_bridge.h'

# Vendored headers fra libad9361 / libiio
mv_if_exists 'ad9361.h'                      'radio-enhet/include/ad9361.h'
mv_if_exists 'iio.h'                         'radio-enhet/include/iio.h'

# The_one.c er identisk med steg38.c — fjern fremfor aa flytte
if [ -e 'The_one.c' ]; then
    if cmp -s 'The_one.c' 'radio-enhet/src/steg38.c' 2>/dev/null; then
        git rm 'The_one.c'
        echo "  The_one.c slettet (identisk med steg38.c)"
    else
        git mv 'The_one.c' 'radio-enhet/tests/The_one.c'
        echo "  The_one.c -> radio-enhet/tests/ (NB: ikke identisk med steg38.c)"
    fi
fi

# Bakke-GUI og GNU Radio (hvis filer finnes i denne klonen)
mv_if_exists 'Bakkestasjon.py'               'bakke-gui/Bakkestasjon.py'
mv_if_exists 'BPSK_Ground_full_system.grc'   'gnuradio/flowgraphs/BPSK_Ground_full_system.grc'

# Deployerbar binaer som lastes opp paa Pluto-radioen
mv_if_exists 'rakett_os'                     'radio-enhet/dist/rakett_os'

# Old code -> archive
if [ -e 'old_code' ]; then
    git mv 'old_code' 'archive/old_code'
    echo "  old_code/  ->  archive/old_code/"
fi

# Tom RIU-mappe — slett (vil bli erstattet av STM32-firmware senere)
if [ -d 'RIU' ] && [ -z "$(ls -A RIU 2>/dev/null)" ]; then
    rmdir RIU
    echo "  Slettet tom RIU/-mappe"
fi

echo ""
echo "== 4/7  Fjerner toolchain/biblioteker/binaer/tarballs fra indeks =="
echo "   (filene blir liggende paa disk; bare ikke sporet lenger)"

to_untrack=(
    'gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf'
    'gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf.tar.xz'
    'sysroot-v0.39.tar.gz'
    'staging'
    'pluto_libs'
    'liquid'
    'liquid-dsp'
)
for p in "${to_untrack[@]}"; do
    git rm -r --cached --quiet --ignore-unmatch "$p" 2>/dev/null || true
    echo "  fjernet fra indeks: $p"
done

echo ""
echo "== 5/7  Skriver oppdatert .gitignore =="
cat > .gitignore <<'EOF'
# ----- Toolchain og sysroot (skal ikke i Git) -----
gcc-linaro-7.3.1-2018.05-x86_64_arm-linux-gnueabihf/
gcc-linaro-*/
staging/

# ----- Nedlastede arkiver (toolchain, sysroot) -----
*.tar.xz
*.tar.gz
*.zip

# ----- Tredjepartsbiblioteker (hentes via package manager / SDK) -----
pluto_libs/
liquid/
liquid-dsp/

# ----- Build-artefakter (C/C++) -----
*.o
*.obj
*.a
*.so
*.so.*
*.dylib
*.elf
*.bin
*.hex
*.map
*.lst
*.d
build/
Build/
Debug/
Release/

# Kompilerte hjelpe-programmer (men IKKE radio-enhet/dist/rakett_os som skal i Git)
framing
test_hardware

# ----- STM32CubeIDE -----
.metadata/
.settings/
.cproject
.project
*.launch
RemoteSystemsTempFiles/
**/Debug/
**/Release/

# ----- Python -----
__pycache__/
*.py[cod]
*$py.class
.venv/
venv/
env/
*.egg-info/
.pytest_cache/
.mypy_cache/

# ----- GNU Radio (behold .grc, ignorer generert .py) -----
gnuradio/flowgraphs/*.py
gnuradio/flowgraphs/*.pyc

# ----- Editor / OS -----
.vscode/
.idea/
*.swp
*.swo
*~
.DS_Store
Thumbs.db

# ----- Logger og maalinger -----
logs/
*.log
captures/
*.iq
*.cfile

# ----- Sensitivt -----
*.pem
*.key
secrets.env
.env
EOF

echo ""
echo "== 6/7  Skriver LICENSE (MIT) =="
year=$(date +%Y)
cat > LICENSE <<EOF
MIT License

Copyright (c) $year Tor Emil Torgersen, Kristian Alexander Brun, Herman Aagotnes

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
EOF

echo ""
echo "== 7/7  Lager commit =="
git add .gitignore LICENSE
git add -A
git commit -m "Restrukturer repo: en mappe per delsystem, MIT-lisens, ekskluder toolchain og biblioteker"

echo ""
echo "== Ferdig =="
echo ""
echo "Verifiser:"
echo "  git log --stat -1 | head -30"
echo "  git ls-tree --name-only HEAD"
echo ""
echo "Push naar du er klar:"
echo "  git push -u origin restruktur"
echo ""
echo "Deretter, paa GitHub: lag PR fra restruktur til main."
