/*
 * Ledger lifecycle.
 *
 * Owns path allocation, initial log creation, version detection/migration,
 * crash recovery, destruction, and the single release path. The durable
 * append ordering itself belongs to commit.c.
 */

#include "internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *atp_ledger_strdup(const char *value) {
    const size_t length = strlen(value);
    char *copy = malloc(length + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, length + 1u);
    return copy;
}

static char *atp_ledger_strdup_suffix(const char *value, const char *suffix) {
    const size_t value_len = strlen(value);
    const size_t suffix_len = strlen(suffix);
    char *copy = malloc(value_len + suffix_len + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, value_len);
    memcpy(copy + value_len, suffix, suffix_len + 1u);
    return copy;
}

static void atp_ledger_release(atp_ledger *ledger) {
    if (!ledger) {
        return;
    }
    if (ledger->log) {
        fclose(ledger->log);
    }
    if (ledger->payloads) {
        for (size_t i = 0u; i < ledger->count; ++i) {
            free(ledger->payloads[i]);
        }
    }
    free(ledger->log_path);
    free(ledger->log_tmp_path);
    free(ledger->off_path);
    free(ledger->off_tmp_path);
    free(ledger->entries);
    free(ledger->payloads);
    free(ledger->payload_lens);
    free(ledger->index);
    free(ledger);
}

atp_ledger *atp_ledger_open(const char *path, atp_status *status) {
    if (status) {
        *status = ATP_OK;
    }
    if (!path || path[0] == '\0') {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return NULL;
    }

    atp_ledger *ledger = calloc(1u, sizeof(*ledger));
    if (!ledger) {
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return NULL;
    }
    ledger->log_path = atp_ledger_strdup(path);
    ledger->log_tmp_path = atp_ledger_strdup_suffix(path, ".tmp");
    ledger->off_path = atp_ledger_strdup_suffix(path, ".off");
    ledger->off_tmp_path = atp_ledger_strdup_suffix(path, ".off.tmp");
    if (!ledger->log_path || !ledger->log_tmp_path || !ledger->off_path || !ledger->off_tmp_path) {
        atp_ledger_release(ledger);
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return NULL;
    }

    remove(ledger->log_tmp_path);
    remove(ledger->off_tmp_path);

    FILE *log = fopen(ledger->log_path, "rb+");
    if (!log) {
        FILE *tmp = fopen(ledger->log_tmp_path, "wb");
        if (!tmp) {
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
        }
        unsigned char header[ATP_LEDGER_HEADER_SIZE];
        header[0] = ATP_LEDGER_FILE_MAGIC_0;
        header[1] = ATP_LEDGER_FILE_MAGIC_1;
        header[2] = ATP_LEDGER_FILE_MAGIC_2;
        header[3] = ATP_LEDGER_FILE_MAGIC_3;
        header[4] = ATP_LEDGER_FILE_MAGIC_4;
        header[5] = ATP_LEDGER_FILE_MAGIC_5;
        header[6] = ATP_LEDGER_FILE_MAGIC_6;
        header[7] = ATP_LEDGER_FILE_MAGIC_7;
        atp_store_u32_le(&header[8], ATPERSON_LEDGER_VERSION);
        const bool ok = fwrite(header, 1u, sizeof(header), tmp) == sizeof(header) &&
                        atp_fsync(tmp) && fclose(tmp) == 0;
        if (!ok) {
            remove(ledger->log_tmp_path);
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
        }
        if (rename(ledger->log_tmp_path, ledger->log_path) != 0) {
            remove(ledger->log_tmp_path);
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
        }
        log = fopen(ledger->log_path, "rb+");
        if (!log) {
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
        }
    }

    unsigned char header[ATP_LEDGER_HEADER_SIZE];
    if (fread(header, 1u, sizeof(header), log) != sizeof(header)) {
        fclose(log);
        atp_ledger_release(ledger);
        if (status) {
            *status = ATP_ERR_FORMAT;
        }
        return NULL;
    }

    const bool is_v1 = header[0] == ATP_LEDGER_FILE_MAGIC_0 &&
                       header[1] == ATP_LEDGER_FILE_MAGIC_1 &&
                       header[2] == ATP_LEDGER_FILE_MAGIC_2 &&
                       header[3] == ATP_LEDGER_FILE_MAGIC_3 &&
                       header[4] == ATP_LEDGER_FILE_MAGIC_4 &&
                       header[5] == ATP_LEDGER_FILE_MAGIC_5 &&
                       header[6] == ATP_LEDGER_FILE_MAGIC_6 &&
                       header[7] == ATP_LEDGER_V1_FILE_MAGIC_7 &&
                       atp_load_u32_le(&header[8]) == ATP_LEDGER_V1_VERSION;
    if (is_v1) {
        fclose(log);
        const atp_status migrated = atp_ledger_migrate_v1(ledger);
        if (migrated != ATP_OK) {
            atp_ledger_release(ledger);
            if (status) {
                *status = migrated;
            }
            return NULL;
        }
        log = fopen(ledger->log_path, "rb+");
        if (!log) {
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_IO;
            }
            return NULL;
        }
        if (fread(header, 1u, sizeof(header), log) != sizeof(header)) {
            fclose(log);
            atp_ledger_release(ledger);
            if (status) {
                *status = ATP_ERR_FORMAT;
            }
            return NULL;
        }
    }

    if (header[0] != ATP_LEDGER_FILE_MAGIC_0 || header[1] != ATP_LEDGER_FILE_MAGIC_1 ||
        header[2] != ATP_LEDGER_FILE_MAGIC_2 || header[3] != ATP_LEDGER_FILE_MAGIC_3 ||
        header[4] != ATP_LEDGER_FILE_MAGIC_4 || header[5] != ATP_LEDGER_FILE_MAGIC_5 ||
        header[6] != ATP_LEDGER_FILE_MAGIC_6 || header[7] != ATP_LEDGER_FILE_MAGIC_7 ||
        atp_load_u32_le(&header[8]) != ATPERSON_LEDGER_VERSION) {
        fclose(log);
        atp_ledger_release(ledger);
        if (status) {
            *status = ATP_ERR_FORMAT;
        }
        return NULL;
    }

    ledger->log = log;
    const atp_status recovered = atp_ledger_recover(ledger);
    if (recovered != ATP_OK) {
        atp_ledger_release(ledger);
        if (status) {
            *status = recovered;
        }
        return NULL;
    }
    return ledger;
}

void atp_ledger_destroy(atp_ledger *ledger) {
    if (!ledger) {
        return;
    }
    if (ledger->log) {
        atp_fsync(ledger->log);
    }
    atp_ledger_release(ledger);
}
