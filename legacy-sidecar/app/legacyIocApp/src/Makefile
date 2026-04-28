TOP = ../..
include $(TOP)/configure/CONFIG

PROD_IOC = legacyIoc

DBD += legacyIoc.dbd
legacyIoc_DBD += base.dbd
legacyIoc_DBD += pvxsIoc.dbd

legacyIoc_SRCS += legacyIoc_registerRecordDeviceDriver.cpp
legacyIoc_SRCS_DEFAULT += legacyMain.cpp

legacyIoc_LIBS += pvxsIoc
legacyIoc_LIBS += pvxs
legacyIoc_LIBS += $(EPICS_BASE_IOC_LIBS)

include $(TOP)/configure/RULES
