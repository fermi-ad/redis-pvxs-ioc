epicsEnvSet("PVXS_QSRV_ENABLE", "YES")

dbLoadDatabase("/opt/legacy-ioc/dbd/legacyIoc.dbd")
legacyIoc_registerRecordDeviceDriver(pdbbase)

epicsEnvSet("IOCNAME", "$(IOCNAME)")
epicsEnvSet("ENGINEER", "$(ENGINEER)")
epicsEnvSet("LOCATION", "$(LOCATION)")
epicsEnvSet("CONTACT", "$(CONTACT)")
epicsEnvSet("BUILDING", "$(BUILDING)")
epicsEnvSet("SECTOR", "$(SECTOR)")

var(reccastTimeout, $(RECCAST_TIMEOUT))
var(reccastMaxHoldoff, $(RECCAST_MAX_HOLDOFF))
addReccasterEnvVars("CONTACT", "BUILDING", "SECTOR")

dbLoadRecords("/opt/legacy-ioc/db/legacy-demo.db", "P=LEGACY:")
dbLoadRecords("/opt/legacy-ioc/db/reccaster.db", "P=LEGACY:RecCaster:")

iocInit()
