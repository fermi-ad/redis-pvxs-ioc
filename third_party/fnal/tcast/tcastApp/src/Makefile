TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#=============================
# Build the IOC application

USR_CPPFLAGS += -DTCAST_API_BUILDING

DBD += tcast.dbd

LIBRARY += tcast

tcast_SRCS += devsup.cpp
tcast_SRCS += receiver.cpp
tcast_SRCS += utils.cpp

tcast_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

