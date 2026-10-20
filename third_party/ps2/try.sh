set -e
cd /w/third_party
FLAGS="-D_EE -G0 -O2 -Wall -Wno-incompatible-pointer-types -DHAVE_CONFIG_H -Ips2 -Ilibdvdread/src -Ilibdvdread/src/dvdread -Ilibdvdnav/src -Ilibdvdnav/src/dvdnav -Ilibdvdnav/src/vm -I$PS2SDK/ee/include -I$PS2SDK/common/include"
mkdir -p /tmp/o
for f in libdvdread/src/*.c libdvdnav/src/*.c libdvdnav/src/vm/*.c; do
  echo "== $f"
  mips64r5900el-ps2-elf-gcc $FLAGS -c $f -o /tmp/o/$(basename $f).o 2>&1 | grep -E "error|warning: implicit" | head -8 || true
done
