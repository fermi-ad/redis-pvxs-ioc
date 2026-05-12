TOP=..

include $(TOP)/configure/CONFIG

# access to private headers
USR_CPPFLAGS += -I$(TOP)/tcastApp/src


TARGETS += $(COMMON_DIR)/testioc.dbd
DBDDEPENDS_FILES += testioc.dbd$(DEP)
TESTFILES += $(COMMON_DIR)/testioc.dbd

testioc_DBD += base.dbd
testioc_DBD += tcast.dbd

TESTPROD_IOC += testioc

testioc_SRCS += testioc.cpp
testioc_SRCS += testioc_registerRecordDeviceDriver.cpp
testioc_SRCS += sim.cpp
TESTS += testioc

PROD_LIBS += tcast
PROD_LIBS += $(EPICS_BASE_IOC_LIBS)

TESTSCRIPTS_HOST += $(TESTS:%=%.t)

include $(TOP)/configure/RULES
