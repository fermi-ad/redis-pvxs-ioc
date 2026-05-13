#!/bin/sh
set -eu

ROOT="${ROOT:-/opt/redis-pvxs-ioc}"
JOBS="${JOBS:-$(nproc)}"

BASE="$ROOT/third_party/epics-base"
PVXS="$ROOT/third_party/pvxs"
SUPPORT="$ROOT/third_party/support"
FNAL="$ROOT/third_party/fnal"

write_release() {
  module="$1"
  shift
  mkdir -p "$module/configure"
  {
    for assignment in "$@"; do
      printf '%s\n' "$assignment"
    done
    printf 'EPICS_BASE=%s\n' "$BASE"
  } > "$module/configure/RELEASE.local"
}

write_config_site() {
  module="$1"
  shift
  mkdir -p "$module/configure"
  {
    for assignment in "$@"; do
      printf '%s\n' "$assignment"
    done
  } > "$module/configure/CONFIG_SITE.local"
}

build_module() {
  module="$1"
  shift
  make -C "$module" "$@" -j"$JOBS"
}

build_dir() {
  dir="$1"
  shift
  make -C "$dir" "$@" -j"$JOBS"
}

generate_streamdevice_dbd() {
  host_arch_script=
  for candidate in "$BASE"/bin/*/EpicsHostArch.pl; do
    host_arch_script="$candidate"
    break
  done
  arch="$("$host_arch_script")"
  common_dir="$STREAM/src/O.Common"
  arch_dir="$STREAM/src/O.$arch"
  stream_records="ao ai bo bi mbbo mbbi mbboDirect mbbiDirect longout longin stringout stringin waveform aai aao calcout lsi lso int64in int64out scalcout"
  stream_base_records="ao ai bo bi mbbo mbbi mbboDirect mbbiDirect longout longin stringout stringin waveform aai aao calcout lsi lso int64in int64out"

  mkdir -p "$common_dir" "$arch_dir"
  (
    cd "$STREAM/src"
    perl makedbd.pl --with-asyn $stream_records > "$common_dir/stream.dbd"
    perl makedbd.pl --with-asyn $stream_base_records > "$common_dir/stream-base.dbd"
    perl makedbd.pl --rec-only scalcout > "$common_dir/stream-scalcout.dbd"
  )
  printf '../O.Common/stream.dbd: ../CONFIG_STREAM\n' > "$arch_dir/stream.dbd.d"
  printf '../O.Common/stream-base.dbd: ../CONFIG_STREAM\n' > "$arch_dir/stream-base.dbd.d"
  printf '../O.Common/stream-scalcout.dbd: ../CONFIG_STREAM\n' > "$arch_dir/stream-scalcout.dbd.d"
}

SEQ="$SUPPORT/seq"
SSCAN="$SUPPORT/sscan"
CALC="$SUPPORT/calc"
ASYN="$SUPPORT/asyn"
STD="$SUPPORT/std"
PCRE="$SUPPORT/pcre"
STREAM="$SUPPORT/StreamDevice"
LUA="$SUPPORT/lua"
IOCSTATS="$SUPPORT/iocStats"
ALIVE="$SUPPORT/alive"
AUTOSAVE="$SUPPORT/autosave"
BUSY="$SUPPORT/busy"
CAPUTLOG="$SUPPORT/caPutLog"
LINSTAT="$SUPPORT/linStat"
TCAST="$FNAL/tcast"
ACNETPV="$FNAL/acnetPV"

write_release "$SEQ"
printf 'EPICS_BASE=%s\n' "$BASE" > "$SEQ/configure/RELEASE"
build_module "$SEQ" configure.install src.install

write_release "$SSCAN" \
  "SUPPORT=$SUPPORT" \
  "SNCSEQ=$SEQ"
build_module "$SSCAN" configure.install sscanApp.install

write_release "$CALC" \
  "SUPPORT=$SUPPORT" \
  "SSCAN=$SSCAN" \
  "SNCSEQ=$SEQ"
build_module "$CALC" configure.install calcApp.install

write_release "$ASYN" \
  "SUPPORT=$SUPPORT" \
  "SNCSEQ=$SEQ" \
  "CALC=$CALC" \
  "SSCAN=$SSCAN"
write_config_site "$ASYN" \
  "LINUX_GPIB=NO" \
  "DRV_USBTMC=NO" \
  "DRV_FTDI=NO" \
  "TIRPC=YES"
build_module "$ASYN" configure.install asyn.install

write_release "$STD" \
  "SUPPORT=$SUPPORT" \
  "SNCSEQ=$SEQ" \
  "ASYN=$ASYN"
build_module "$STD" configure.install stdApp.install

write_release "$PCRE"
build_module "$PCRE" configure.install pcre.install

write_release "$STREAM" \
  "SUPPORT=$SUPPORT" \
  "ASYN=$ASYN" \
  "CALC=$CALC" \
  "SSCAN=$SSCAN" \
  "SNCSEQ=$SEQ" \
  "PCRE=$PCRE"
write_config_site "$STREAM" \
  "TIRPC=NO"
build_module "$STREAM" configure.install
generate_streamdevice_dbd
build_module "$STREAM" src.install

write_release "$LUA" \
  "SUPPORT=$SUPPORT" \
  "ASYN=$ASYN"
build_module "$LUA" configure.install
build_dir "$LUA/luaApp/src" install PROD_IOC_DEFAULT=
build_dir "$LUA/luaApp/Db" install

write_release "$IOCSTATS" \
  "SUPPORT=$SUPPORT" \
  "SNCSEQ=$SEQ"
write_config_site "$IOCSTATS" \
  "MAKE_TEST_IOC_APP=NO"
{
  printf 'MAKE_TEST_IOC_APP=NO\n'
  printf 'SUPPORT=%s\n' "$SUPPORT"
  printf 'SNCSEQ=%s\n' "$SEQ"
  printf 'EPICS_BASE=%s\n' "$BASE"
} > "$IOCSTATS/configure/RELEASE"
build_module "$IOCSTATS" configure.install devIocStats.install iocAdmin.install

write_release "$ALIVE" \
  "SUPPORT=$SUPPORT"
build_module "$ALIVE" configure.install
build_dir "$ALIVE/aliveApp/src" install PROD_IOC=
build_dir "$ALIVE/aliveApp/Db" install

write_release "$AUTOSAVE" \
  "SUPPORT=$SUPPORT"
build_module "$AUTOSAVE" configure.install
build_dir "$AUTOSAVE/asApp/src" install PROD_IOC= PROD_HOST=
build_dir "$AUTOSAVE/asApp/Db" install

write_release "$BUSY" \
  "SUPPORT=$SUPPORT" \
  "ASYN=$ASYN" \
  "AUTOSAVE=$AUTOSAVE" \
  "BUSY=$BUSY"
build_module "$BUSY" configure.install
build_dir "$BUSY/busyApp/src" install PROD_IOC=
build_dir "$BUSY/busyApp/Db" install

write_release "$CAPUTLOG"
build_module "$CAPUTLOG" configure.install caPutLogApp.install

write_release "$LINSTAT"
write_config_site "$LINSTAT" \
  "LINSTAT_BUILD_EXAMPLE=NO"
build_module "$LINSTAT" configure.install statApp.install

write_release "$TCAST"
write_config_site "$TCAST" \
  "BUILD_DEMO=NO"
build_module "$TCAST" configure.install tcastApp.install

write_release "$ACNETPV" \
  "PVXS=$PVXS"
build_module "$ACNETPV" configure.install acnetPVApp.install
