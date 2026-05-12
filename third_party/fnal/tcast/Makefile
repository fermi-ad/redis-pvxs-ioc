# Makefile at top of application tree
TOP = .
include $(TOP)/configure/CONFIG

# Directories to build, any order
DIRS += configure
DIRS += tcastApp
ifeq ($(BUILD_DEMO),YES)
DIRS += tcastDemoApp
endif
DIRS += test

tcastApp_DEPEND_DIRS += configure
tcastDemoApp_DEPEND_DIRS += tcastApp
test_DEPEND_DIRS += tcastApp

# iocBoot depends on all *App dirs
iocBoot_DEPEND_DIRS += $(filter %App,$(DIRS))

# Add any additional dependency rules here:

include $(TOP)/configure/RULES_TOP
