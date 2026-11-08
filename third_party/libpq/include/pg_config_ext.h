/*
 * Minimal pg_config_ext.h for the vendored libpq client headers.
 *
 * Upstream generates this file at build time; the only macro the public
 * headers need is PG_INT64_TYPE.  We target LP64 platforms (Linux x86_64 /
 * aarch64), where long is 64-bit.  If you build on Windows/LLP64, change
 * this to "long long".
 */
#ifndef PG_CONFIG_EXT_H
#define PG_CONFIG_EXT_H

#define PG_INT64_TYPE long int

#endif  /* PG_CONFIG_EXT_H */
