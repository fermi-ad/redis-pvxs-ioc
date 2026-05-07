#!/bin/sh
set -eu

append_ignored_server() {
  case " ${EPICS_IOC_IGNORE_SERVERS:-} " in
    *" $1 "*) ;;
    *) export EPICS_IOC_IGNORE_SERVERS="${EPICS_IOC_IGNORE_SERVERS:+$EPICS_IOC_IGNORE_SERVERS }$1" ;;
  esac
}

case "${LEGACY_IOC_ENABLE_CA:-NO}" in
  YES)
    export EPICS_CA_AUTO_ADDR_LIST="${EPICS_CA_AUTO_ADDR_LIST:-NO}"
    export EPICS_CA_ADDR_LIST="${EPICS_CA_ADDR_LIST:-239.128.1.6}"
    export EPICS_CA_MCAST_TTL="${EPICS_CA_MCAST_TTL:-32}"

    export EPICS_CAS_INTF_ADDR_LIST="${EPICS_CAS_INTF_ADDR_LIST:-239.128.1.6}"
    export EPICS_CAS_AUTO_BEACON_ADDR_LIST="${EPICS_CAS_AUTO_BEACON_ADDR_LIST:-NO}"
    export EPICS_CAS_BEACON_ADDR_LIST="${EPICS_CAS_BEACON_ADDR_LIST:-239.128.1.7}"
    ;;
  NO)
    append_ignored_server rsrv
    ;;
  *)
    echo "LEGACY_IOC_ENABLE_CA must be YES or NO" >&2
    exit 64
    ;;
esac

export EPICS_PVA_AUTO_ADDR_LIST="${EPICS_PVA_AUTO_ADDR_LIST:-NO}"
PVA_ADDR_DEFAULT="239.128.1.6"
if [ -n "${EPICS_HOST_INTERFACE:-}" ]; then
  PVA_ADDR_DEFAULT="${PVA_ADDR_DEFAULT},8@${EPICS_HOST_INTERFACE} ${PVA_ADDR_DEFAULT}"
fi
export EPICS_PVA_ADDR_LIST="${EPICS_PVA_ADDR_LIST:-$PVA_ADDR_DEFAULT}"

export EPICS_PVAS_INTF_ADDR_LIST="${EPICS_PVAS_INTF_ADDR_LIST:-239.128.1.6}"
export EPICS_PVAS_AUTO_BEACON_ADDR_LIST="${EPICS_PVAS_AUTO_BEACON_ADDR_LIST:-NO}"
export EPICS_PVAS_BEACON_ADDR_LIST="${EPICS_PVAS_BEACON_ADDR_LIST:-239.128.1.7,10}"

export IOCNAME="${IOCNAME:-legacy-ioc}"
export ENGINEER="${ENGINEER:-redis-pvxs-ioc}"
export LOCATION="${LOCATION:-redis-pvxs-ioc-demo}"
export CONTACT="${CONTACT:-redis-pvxs-ioc}"
export BUILDING="${BUILDING:-demo}"
export SECTOR="${SECTOR:-demo}"
export RECCAST_TIMEOUT="${RECCAST_TIMEOUT:-20}"
export RECCAST_MAX_HOLDOFF="${RECCAST_MAX_HOLDOFF:-10}"

if [ "$#" -eq 0 ]; then
  set -- /etc/legacy-ioc/st.cmd
fi

exec /opt/legacy-ioc/bin/legacyIoc "$@"
