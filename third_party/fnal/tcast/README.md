# TCAST (Tclk multiCAST) driver

EPICS Driver to listen for TCLK multicast packets
and trigger record processing.

## Building

Builds as any EPICS support module.
Requires EPICS Base >= 7.0.3

```
git clone https://PLACEHOLDER/tcast.git
cd tcast
cat <<EOF > configure/RELEASE.local
EPICS_BASE=/path/to/epics/base
EOF
cat <<EOF > configure/CONFIG_SITE.local
# optional
BUILD_DEMO=YES
EOF
make
# optional, run unit test
make runtests
```

### Including in an IOC

The tcast driver provides `tcast.dbd` and `libtcast.so`.
Include in an IOC application `Makefile` like:

```
DBD = myiocapp.dbd
myiocapp_DBD  = base.dbd
myiocapp_DBD += tcast.dbd  # <<<< Add

PROD_IOC = myiocapp
myiocapp_SRCS = tcastDemo_registerRecordDeviceDriver.cpp
myiocapp_SRCS_DEFAULT += myiocappMain.cpp
myiocapp_LIBS += tcast     # <<<< Add
myiocapp_LIBS += $(EPICS_BASE_IOC_LIBS)
```

See `tcastDemoApp/src/Makefile` for a full example.

## Configuration

The tcast driver does nothing unless one or more records with device support are loaded.
(eg. some `DTYP=TCLK *`)
These device supports take configuration from three locations.

1. The `$TCAST_DEFAULT` environment variable.
2. An `info(tclk:conf, "...")` within a `record(...) { ... }` block.
3. The `INP` or `OUT` field string

A complete configuration is the concatenation of all three strings with a space between each.
So the configuration parameters listed below may appear in any, or all,
of these three locations.
Later parameters override earlier.
eg. a `group=1.2.3.4` in `$TCAST_DEFAULT` will be overridden by a `group=4.3.2.1` in `INP`.

`INP` or `OUT` link strings are of the `INST_IO` format and must be prefixed with a `@`.

Extended device support is provided for `DTYP=TCLK *`,
allowing `INP` or `OUT` to be changed after `iocInit()`.

### Parameters

`group=239.128.5.4:50090`

Specifies the multicast group, and optionally port (default 50090), to listen for.

`iface=1.2.3.4`

A local interface address on which to listen.
If omitted, defaults to wildcard `0.0.0.0`.

`event=42`

For `DTYP="TCLK Event"`, this is the event code which will cause processing.
`event=` may be omitted to abbreviate as eg. `42`.

`counter=rx` or `=err`, `=tmo`, `=skip`, `=pb`

For `DTYP="TCLK Counter"`, Select a diagnostic counter to read.

### `DTYP="TCLK Event"`

```
record(longin, "rec:name") {
    field(DTYP, "TCLK Event")
    # configuration string fragment from $TCAST_DEFAULT implicitly used
    info(tclk:conf, "event=42") # configuration string fragment
    field(INP , "@event=43")    # configuration string fragment (changeable at runtime)
    # event=43 takes precedent
    field(SCAN, "I/O Intr")
    field(TSE , "-2")
}
```

The `VAL` will be the event sequence number.

The installed `tcastEvent.db` provides one record instance,
and a periodic sequence counter rate measurement.

### `DTYP="TCLK Counter"`

```
record(longin, "rec:name") {
    field(DTYP, "TCLK Counter")
    # configuration string fragment from $TCAST_DEFAULT implicitly used
    field(INP , "counter=err")    # configuration string fragment (changeable at runtime)
    info(tclk:conf, "counter=rx") # configuration string fragment
    # counter=err takes precedent
    field(SCAN, "1 second")
}
```

The installed `tcastHealth.db` provides a full set of diagnostic counters,
each with a count rate measurement.

## Diagnostics

Run `dbior` and look for `tcastdrv`.

A support database `tcastHealth.db` can be loaded to provide remote
access to the same status information shown by `dbior`.
