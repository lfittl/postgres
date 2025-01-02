# contrib/pg_stat_plans/Makefile

MODULE_big = pg_stat_plans
OBJS = \
	$(WIN32RES) \
	pg_stat_plans.o

EXTENSION = pg_stat_plans
DATA = pg_stat_plans--1.0.sql
PGFILEDESC = "pg_stat_plans - track per-plan call counts, execution times and EXPLAIN texts"

LDFLAGS_SL += $(filter -lm, $(LIBS))

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/pg_stat_plans/pg_stat_plans.conf
REGRESS = select privileges cleanup
# Disabled because these tests require "shared_preload_libraries=pg_stat_plans",
# which typical installcheck users do not have (e.g. buildfarm clients).
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_stat_plans
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
