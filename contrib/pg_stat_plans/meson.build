# Copyright (c) 2024, PostgreSQL Global Development Group

pg_stat_plans_sources = files(
  'pg_stat_plans.c',
  'jumblefuncs.c',
)

if host_system == 'windows'
  pg_stat_plans_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_stat_plans',
    '--FILEDESC', 'pg_stat_plans - track per-plan call counts, execution times and EXPLAIN texts',])
endif

pg_stat_plans = shared_module('pg_stat_plans',
  pg_stat_plans_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += pg_stat_plans

install_data(
  'pg_stat_plans.control',
  'pg_stat_plans--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_stat_plans',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'select',
      'privileges',
      'cleanup',
    ],
    'regress_args': ['--temp-config', files('pg_stat_plans.conf')],
    # Disabled because these tests require
    # "shared_preload_libraries=pg_stat_plans", which typical
    # runningcheck users do not have (e.g. buildfarm clients).
    'runningcheck': false,
  }
}
