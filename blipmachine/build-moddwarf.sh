#!/bin/sh
# Cross-build per MOD Dwarf (aarch64, Cortex-A35) senza mod-plugin-builder.
# Usa zig come cross-compiler: linka contro glibc 2.17 (compatibile col Dwarf)
# e include staticamente la libreria C++, quindi non dipende dalla libstdc++ del device.
# Requisiti: python3 + `pip install ziglang`
set -e
cd "$(dirname "$0")"

make clean >/dev/null 2>&1 || true
TC="$PWD/build/zigtc"
mkdir -p "$TC"
for t in cc c++ ar; do
  case $t in
    cc)  n=cc;  a="cc -target aarch64-linux-gnu.2.17 -mcpu=cortex_a35" ;;
    c++) n=cxx; a="c++ -target aarch64-linux-gnu.2.17 -mcpu=cortex_a35" ;;
    ar)  n=ar;  a="ar" ;;
  esac
  printf '#!/bin/sh\nexec python3 -m ziglang %s "$@"\n' "$a" > "$TC/$n"
  chmod +x "$TC/$n"
done

# -z nodelete: la .so resta mappata dopo dlclose, cosi' i distruttori statici
# della libc++ inclusa non vengono eseguiti su codice gia' smappato all'uscita del processo.
CC="$TC/cc" CXX="$TC/cxx" AR="$TC/ar" LDFLAGS="-Wl,-z,nodelete" \
  make NOOPT=true CROSS_COMPILING=true 2>&1 | grep -vE "nullability|^\s|In file included|dumpmachine|is invalid" || true

test -f bin/blipmachine.lv2/blipmachine_dsp.so
mkdir -p dist
tar -C bin -czf dist/blipmachine-moddwarf.tar.gz blipmachine.lv2
echo "OK -> dist/blipmachine-moddwarf.tar.gz"
