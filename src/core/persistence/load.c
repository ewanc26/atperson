#include "persistence/persistence_internal.h"

/*
 * Public snapshot load dispatch.
 *
 * This atom owns file I/O, magic/version dispatch, and the lifetime of the
 * raw snapshot image. Version-specific decoding belongs in v5.c (v5), v6.c
 * (v6) and migration.c (v4). Unsupported historical versions fail
 * explicitly.
 */
atp_graph *atp_graph_load(const char *path, atp_status *status) {
    if (status) {
        *status = ATP_OK;
    }
    if (!path || path[0] == '\0') {
        if (status) {
            *status = ATP_ERR_INVALID_ARGUMENT;
        }
        return NULL;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }
    const long file_size_long = ftell(file);
    if (file_size_long < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }
    const size_t file_size = (size_t)file_size_long;

    if (file_size < 8u + 4u + 12u + 8u) {
        fclose(file);
        if (status) {
            *status = ATP_ERR_FORMAT;
        }
        return NULL;
    }

    unsigned char *data = malloc(file_size);
    if (!data) {
        fclose(file);
        if (status) {
            *status = ATP_ERR_OUT_OF_MEMORY;
        }
        return NULL;
    }
    if (fread(data, 1u, file_size, file) != file_size) {
        free(data);
        fclose(file);
        if (status) {
            *status = ATP_ERR_IO;
        }
        return NULL;
    }
    fclose(file);

    atp_graph *graph = NULL;
    if (memcmp(data, ATP_SNAPSHOT_MAGIC_V7, 8u) == 0) {
        const uint32_t version = atp_load_u32le(data + 8u);
        if (version != ATPERSON_SNAPSHOT_VERSION) {
            free(data);
            if (status) {
                *status = ATP_ERR_FORMAT;
            }
            return NULL;
        }
        graph = atp_load_v7(data, file_size, status);
    } else if (memcmp(data, ATP_SNAPSHOT_MAGIC_V6, 8u) == 0) {
        const uint32_t version = atp_load_u32le(data + 8u);
        if (version != ATPERSON_SNAPSHOT_VERSION_V6) {
            free(data);
            if (status) {
                *status = ATP_ERR_FORMAT;
            }
            return NULL;
        }
        graph = atp_load_v6(data, file_size, status);
    } else if (memcmp(data, ATP_SNAPSHOT_MAGIC_V5, 8u) == 0) {
        const uint32_t version = atp_load_u32le(data + 8u);
        if (version != ATPERSON_SNAPSHOT_VERSION_V5) {
            free(data);
            if (status) {
                *status = ATP_ERR_FORMAT;
            }
            return NULL;
        }
        graph = atp_load_v5(data, file_size, status);
    } else if (memcmp(data, ATP_SNAPSHOT_MAGIC_V4, 8u) == 0) {
        const uint32_t version = atp_load_u32le(data + 8u);
        if (version != ATPERSON_SNAPSHOT_VERSION_V4) {
            free(data);
            if (status) {
                *status = ATP_ERR_FORMAT;
            }
            return NULL;
        }
        graph = atp_load_v4(data, file_size, status);
    } else {
        free(data);
        if (status) {
            *status = ATP_ERR_FORMAT;
        }
        return NULL;
    }

    free(data);
    return graph;
}
