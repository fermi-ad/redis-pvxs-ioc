TOP = ../..
include $(TOP)/configure/CONFIG

PROD_IOC = legacyIoc

DBD += legacyIoc.dbd
legacyIoc_DBD += base.dbd
legacyIoc_DBD += pvxsIoc.dbd
legacyIoc_DBD += reccaster.dbd
legacyIoc_DBD += sscanSupport.dbd
legacyIoc_DBD += sscanProgressSupport.dbd
legacyIoc_DBD += calcSupport.dbd
legacyIoc_DBD += asyn.dbd
legacyIoc_DBD += asynCalc.dbd
legacyIoc_DBD += drvAsynIPPort.dbd
legacyIoc_DBD += drvAsynSerialPort.dbd
legacyIoc_DBD += stdSupport.dbd
legacyIoc_DBD += stream.dbd
legacyIoc_DBD += luaSupport.dbd
legacyIoc_DBD += devIocStats.dbd
legacyIoc_DBD += iocAdmin.dbd
legacyIoc_DBD += aliveSupport.dbd
legacyIoc_DBD += asSupport.dbd
legacyIoc_DBD += busySupport.dbd
legacyIoc_DBD += caPutLog.dbd
legacyIoc_DBD += caPutJsonLog.dbd
legacyIoc_DBD += tcast.dbd
legacyIoc_DBD += acnetPV.dbd

legacyIoc_SRCS += legacyIoc_registerRecordDeviceDriver.cpp
legacyIoc_SRCS_DEFAULT += legacyMain.cpp

legacyIoc_LIBS += acnetPV
legacyIoc_LIBS += tcast
legacyIoc_LIBS += caPutLog
legacyIoc_LIBS += busy
legacyIoc_LIBS += autosave
legacyIoc_LIBS += alive
legacyIoc_LIBS += devIocStats
legacyIoc_LIBS += lua
legacyIoc_LIBS += stream
legacyIoc_LIBS += pcre
legacyIoc_LIBS += std
legacyIoc_LIBS += asyn
legacyIoc_LIBS += calc
legacyIoc_LIBS += scanProgress
legacyIoc_LIBS += sscan
legacyIoc_LIBS += seq
legacyIoc_LIBS += pv
legacyIoc_LIBS += reccaster
legacyIoc_LIBS += pvxsIoc
legacyIoc_LIBS += pvxs
legacyIoc_LIBS += $(EPICS_BASE_IOC_LIBS)

include $(TOP)/configure/RULES
