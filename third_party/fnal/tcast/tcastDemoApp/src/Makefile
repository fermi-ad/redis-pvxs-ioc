TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#=============================
# Build the IOC application

PROD_IOC = tcastDemo
# tcastDemo.dbd will be created and installed
DBD += tcastDemo.dbd

# tcastDemo.dbd will be made up from these files:
tcastDemo_DBD += base.dbd

# Include dbd files from all support applications:
tcastDemo_DBD += tcast.dbd

# Add all the support libraries needed by this IOC
tcastDemo_LIBS += tcast

# tcastDemo_registerRecordDeviceDriver.cpp derives from tcastDemo.dbd
tcastDemo_SRCS += tcastDemo_registerRecordDeviceDriver.cpp

# Build the main IOC entry point on workstation OSs.
tcastDemo_SRCS_DEFAULT += tcastDemoMain.cpp
tcastDemo_SRCS_vxWorks += -nil-

# Add support from base/src/vxWorks if needed
#tcastDemo_OBJS_vxWorks += $(EPICS_BASE_BIN)/vxComLibrary

# Finally link to the EPICS Base libraries
tcastDemo_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

