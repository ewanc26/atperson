/*
 * atperson bootstrap: first-run environment preparation.
 *
 * When the data directory (~/.ewanc26/atperson) or the .env file does not
 * exist, this module creates both before any other command runs:
 *
 *   - the data directory (mode 0700, owner-only);
 *   - a .env template listing every environment variable atperson reads,
 *     with empty values and inline documentation, mode 0600.
 *
 * Everything is plain C23 in the core layer: no learning logic, no C++, no
 * network. The runtime (C++) calls atp_bootstrap_home() early in main() and
 * prints the returned notice so the first run is self-explanatory. The
 * bootstrap is idempotent: an existing directory or .env is never touched.
 */

#include "atperson/bootstrap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ATP_BOOTSTRAP_NOTICE_BYTES 512

static const char *const atp_env_template =
    "# atperson environment\n"
    "#\n"
    "# This file was created automatically on first run. atperson reads these\n"
    "# values from the environment; it does not parse this file itself.\n"
    "# Fill it in and source it (or export the variables) before running\n"
    "# `atperson sync` or `atperson cursor`:\n"
    "#\n"
    "#   set -a; . \"$HOME/.ewanc26/atperson/.env\"; set +a\n"
    "#\n"
    "# Do not commit this file. Mode 0600.\n"
    "\n"
    "# Account credentials for timeline sync (required for sync/cursor).\n"
    "ATPERSON_IDENTIFIER=\n"
    "ATPERSON_APP_PASSWORD=\n"
    "\n"
    "# PDS/service URL (optional, default https://bsky.social).\n"
    "ATPERSON_SERVICE=\n"
    "\n"
    "# Data directory override (optional, default ~/.ewanc26/atperson).\n"
    "ATPERSON_HOME=\n"
    "\n"
    "# Per-path overrides (optional; defaults live under the data directory).\n"
    "ATPERSON_STATE=\n"
    "ATPERSON_LEDGER=\n"
    "ATPERSON_INGESTION_STATE=\n"
    "\n"
    "# Runtime resource overrides. Empty or 0 keeps automatic host/container\n"
    "# budgeting; see `atperson resources` and docs/resources.md.\n"
    "ATPERSON_MEMORY_BUDGET_BYTES=\n"
    "ATPERSON_DISK_RESERVE_BYTES=\n"
    "ATPERSON_NODE_CAPACITY=\n"
    "ATPERSON_EDGE_CAPACITY=\n"
    "ATPERSON_SYNC_PAGE_SIZE=\n"
    "ATPERSON_SYNC_MAX_OBSERVATIONS=\n";

static int atp_mkdir_p(const char *path) {
    /* mkdir -p in C. Walk the path creating each missing component. */
    char buffer[4096];
    const size_t length = strlen(path);
    if (length == 0 || length >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, path, length + 1);

    for (char *cursor = buffer + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (mkdir(buffer, 0700) != 0 && errno != EEXIST) {
            return -1;
        }
        *cursor = '/';
    }
    return mkdir(buffer, 0700);
}

static int atp_write_env_file(const char *path) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }
    const size_t length = strlen(atp_env_template);
    const ssize_t written = write(fd, atp_env_template, length);
    const int saved_errno = errno;
    if (close(fd) != 0 && written >= 0) {
        return -1;
    }
    if (written < 0) {
        errno = saved_errno;
        return -1;
    }
    if ((size_t)written != length) {
        errno = EIO;
        return -1;
    }
    return 0;
}

atp_status atp_bootstrap_home(const char *home_directory, char *notice,
                              size_t notice_bytes) {
    if (home_directory == NULL || home_directory[0] == '\0') {
        return ATP_ERR_INVALID_ARGUMENT;
    }
    if (notice != NULL && notice_bytes > 0) {
        notice[0] = '\0';
    }

    struct stat info;
    const bool have_dir = stat(home_directory, &info) == 0;
    if (have_dir && !S_ISDIR(info.st_mode)) {
        return ATP_ERR_IO;
    }

    /* Create the data directory when missing. */
    bool created_dir = false;
    if (!have_dir) {
        if (atp_mkdir_p(home_directory) != 0) {
            return ATP_ERR_IO;
        }
        created_dir = true;
    }

    /* Create the .env template when missing. */
    char env_path[4096];
    int written = snprintf(env_path, sizeof(env_path), "%s/.env", home_directory);
    if (written < 0 || (size_t)written >= sizeof(env_path)) {
        return ATP_ERR_IO;
    }

    bool created_env = false;
    if (stat(env_path, &info) != 0) {
        if (errno != ENOENT || atp_write_env_file(env_path) != 0) {
            return ATP_ERR_IO;
        }
        created_env = true;
    }

    if (notice != NULL && (created_dir || created_env)) {
        if (created_dir && created_env) {
            snprintf(notice, notice_bytes,
                     "created data directory %s and .env template; fill in "
                     "ATPERSON_IDENTIFIER and ATPERSON_APP_PASSWORD before running sync",
                     home_directory);
        } else if (created_dir) {
            snprintf(notice, notice_bytes, "created data directory %s", home_directory);
        } else {
            snprintf(notice, notice_bytes,
                     "created .env template at %s; fill in ATPERSON_IDENTIFIER and "
                     "ATPERSON_APP_PASSWORD before running sync",
                     env_path);
        }
    }
    return ATP_OK;
}

const char *atp_default_home_directory(char *out_buffer, size_t buffer_bytes,
                                        size_t *required_bytes) {
    if (out_buffer == NULL || buffer_bytes == 0) {
        if (required_bytes != NULL) {
            *required_bytes = 0;
        }
        return NULL;
    }

    const char *override = getenv("ATPERSON_HOME");
    if (override != NULL && override[0] != '\0') {
        const size_t length = strlen(override);
        if (required_bytes != NULL) {
            *required_bytes = length + 1;
        }
        if (length + 1 > buffer_bytes) {
            return NULL;
        }
        memcpy(out_buffer, override, length + 1);
        return out_buffer;
    }

    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        if (required_bytes != NULL) {
            *required_bytes = sizeof(".atperson");
        }
        if (buffer_bytes < sizeof(".atperson")) {
            return NULL;
        }
        memcpy(out_buffer, ".atperson", sizeof(".atperson"));
        return out_buffer;
    }

    const int written = snprintf(out_buffer, buffer_bytes, "%s/.ewanc26/atperson", home);
    if (required_bytes != NULL) {
        *required_bytes = (written < 0) ? 0 : (size_t)written + 1;
    }
    if (written < 0 || (size_t)written >= buffer_bytes) {
        return NULL;
    }
    return out_buffer;
}
