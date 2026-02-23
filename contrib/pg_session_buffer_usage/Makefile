# contrib/pg_session_buffer_usage/Makefile

MODULE_big = pg_session_buffer_usage
OBJS = \
	$(WIN32RES) \
	pg_session_buffer_usage.o

EXTENSION = pg_session_buffer_usage
DATA = pg_session_buffer_usage--1.0.sql
PGFILEDESC = "pg_session_buffer_usage - show buffer usage statistics for the current session"

REGRESS = pg_session_buffer_usage

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_session_buffer_usage
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
