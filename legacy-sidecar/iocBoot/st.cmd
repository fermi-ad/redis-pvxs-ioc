epicsEnvSet("PVXS_QSRV_ENABLE", "YES")

dbLoadDatabase("/opt/legacy-ioc/dbd/legacyIoc.dbd")
legacyIoc_registerRecordDeviceDriver(pdbbase)

dbLoadRecords("/opt/legacy-ioc/db/legacy-demo.db", "P=LEGACY:")

iocInit()
