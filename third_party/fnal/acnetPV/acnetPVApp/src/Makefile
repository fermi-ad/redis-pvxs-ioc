TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#==================================================
# build a support library
LIBRARY_IOC += acnetPV

# xxxRecord.h will be created from xxxRecord.dbd

# install acnetPV.dbd into <top>/dbd
DBD += acnetPV.dbd

# pvData header files
#INC += acnetPV.h
INC += acnetPVXS.h

USR_CPPFLAGS += -DUSE_TYPED_RSET
USR_CXXFLAGS_DEFAULT += -fno-operator-names
USR_CXXFLAGS_DEFAULT += -pedantic

# specify all source files to be compiled and added to the library
acnetPV_SRCS += acnetPV_registerRecordDeviceDriver.cpp
#acnetPV_SRCS += acnetPV.cpp
acnetPV_SRCS += acnetPVXS.cpp
acnetPV_SRCS += CreateAcnetPVRegister.cpp

acnetPV_LIBS += pvxs

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE
