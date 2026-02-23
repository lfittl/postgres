# Copyright (c) 2026, PostgreSQL Global Development Group

pg_session_buffer_usage_sources = files(
  'pg_session_buffer_usage.c',
)

if host_system == 'windows'
  pg_session_buffer_usage_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_session_buffer_usage',
    '--FILEDESC', 'pg_session_buffer_usage - show buffer usage statistics for the current session',])
endif

pg_session_buffer_usage = shared_module('pg_session_buffer_usage',
  pg_session_buffer_usage_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_session_buffer_usage

install_data(
  'pg_session_buffer_usage--1.0.sql',
  'pg_session_buffer_usage.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_session_buffer_usage',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_session_buffer_usage',
    ],
  },
}
