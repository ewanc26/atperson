#ifndef ATPERSON_BOOTSTRAP_H
#define ATPERSON_BOOTSTRAP_H

#include "atperson/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * First-run bootstrap. Creates the data directory (mode 0700) and a .env
 * template (mode 0600) when either is missing. Idempotent: existing files
 * and directories are never modified.
 *
 * home_directory: absolute or relative path to the data directory.
 * notice: optional buffer (use ATP_BOOTSTRAP_NOTICE_BYTES) receiving a
 *   human-readable description of what was created, or an empty string when
 *   nothing needed creating. May be NULL.
 *
 * Returns ATP_OK, ATP_ERR_INVALID_ARGUMENT for an empty path, or ATP_ERR_IO
 * when the directory exists as a non-directory or creation fails.
 */
#define ATP_BOOTSTRAP_NOTICE_BYTES 512
atp_status atp_bootstrap_home(const char *home_directory, char *notice,
                              size_t notice_bytes);

/*
 * Resolves the data directory atperson should bootstrap into out_buffer:
 * $ATPERSON_HOME when set, else $HOME/.ewanc26/atperson, else ".atperson"
 * in the working directory. Mirrors the runtime's data_dir() logic so the
 * C bootstrap and the C++ runtime always agree on the location.
 *
 * Returns out_buffer, or NULL when the buffer is too small (the required
 * size, including the terminator, is written to required_bytes when
 * non-NULL).
 */
const char *atp_default_home_directory(char *out_buffer, size_t buffer_bytes,
                                       size_t *required_bytes);

#ifdef __cplusplus
}
#endif

#endif /* ATPERSON_BOOTSTRAP_H */
