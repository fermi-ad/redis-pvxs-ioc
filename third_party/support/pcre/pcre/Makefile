TOP=..
include $(TOP)/configure/CONFIG
LIBRARY = pcre

LIB_SRCS += pcre_byte_order.c
LIB_SRCS += pcre_chartables.c
LIB_SRCS += pcre_compile.c
LIB_SRCS += pcre_config.c
LIB_SRCS += pcre_dfa_exec.c
LIB_SRCS += pcre_exec.c
LIB_SRCS += pcre_fullinfo.c
LIB_SRCS += pcre_get.c
LIB_SRCS += pcre_globals.c
LIB_SRCS += pcre_jit_compile.c
LIB_SRCS += pcre_maketables.c
LIB_SRCS += pcre_newline.c
LIB_SRCS += pcre_ord2utf8.c
LIB_SRCS += pcre_refcount.c
LIB_SRCS += pcre_string_utils.c
LIB_SRCS += pcre_study.c
LIB_SRCS += pcre_tables.c
LIB_SRCS += pcre_ucd.c
LIB_SRCS += pcre_valid_utf8.c
LIB_SRCS += pcre_version.c
LIB_SRCS += pcre_xclass.c

INC += pcre.h

include $(TOP)/configure/RULES

#for pcre version 8 and higher: have to create pcre.h and define some variables
USR_CPPFLAGS += -DHAVE_MEMMOVE=1 -DNEWLINE=10
USR_CPPFLAGS += -DLINK_SIZE=2 -DMAX_NAME_COUNT=10000 -DMAX_NAME_SIZE=32
USR_CPPFLAGS += -DMATCH_LIMIT=10000000 -DMATCH_LIMIT_RECURSION=MATCH_LIMIT
USR_CPPFLAGS += -DPOSIX_MALLOC_THRESHOLD=10
USR_CPPFLAGS += -DHAVE_CONFIG_H

../O.Common/%.h: ../%.h.generic
	$(CP) $< $@
