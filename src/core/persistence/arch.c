#include "persistence/persistence_internal.h"

/*
 * Snapshot architecture metadata probe (issue #73).
 *
 * Reads only the persisted neural architecture descriptor from a snapshot
 * file without materializing the graph. Rebuild uses this to recover the
 * model-generation topology from durable metadata — it must never replan
 * topology from host hardware — and the runtime preflights a persisted
 * architecture against current memory headroom with it.
 *
 * The walk is seek-based and bounded: every section is
 * `tag u32le | length u64le | payload`, so a non-ARCH payload is skipped by
 * seeking past its declared length without buffering it. Integrity is fully
 * validated by the whole-file loaders in v6.c/v5.c/migration.c; this probe is
 * deliberately partial and validates only the descriptor it returns. v4/v5
 * snapshots carry no descriptor and report the legacy architecture, matching
 * how they load.
 *
 * Failure modes: NULL arguments -> ATP_ERR_INVALID_ARGUMENT; an unreadable or
 * truncated file -> ATP_ERR_IO; a file that is not a v4/v5/v6 snapshot, a
 * malformed section walk, or a structurally invalid descriptor ->
 * ATP_ERR_FORMAT. Callers own `*out_architecture` only on ATP_OK.
 */
atp_status atp_snapshot_neural_architecture(const char *path,
                                            atp_neural_architecture *out_architecture) {
    if (!path || path[0] == '\0' || !out_architecture) {
        return ATP_ERR_INVALID_ARGUMENT;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        return ATP_ERR_IO;
    }
    if (fseek(file, 0L, SEEK_END) != 0) {
        fclose(file);
        return ATP_ERR_IO;
    }
    const long size_long = ftell(file);
    if (size_long < 0 || fseek(file, 0L, SEEK_SET) != 0) {
        fclose(file);
        return ATP_ERR_IO;
    }
    const size_t size = (size_t)size_long;
    if (size < 12u) {
        fclose(file);
        return ATP_ERR_FORMAT;
    }

    char magic[8];
    unsigned char version_bytes[4];
    if (fread(magic, 1u, sizeof(magic), file) != sizeof(magic) ||
        fread(version_bytes, 1u, sizeof(version_bytes), file) != sizeof(version_bytes)) {
        fclose(file);
        return ATP_ERR_IO;
    }
    bool v6 = memcmp(magic, ATP_SNAPSHOT_MAGIC_V6, sizeof(magic)) == 0;
    const bool v5 = memcmp(magic, ATP_SNAPSHOT_MAGIC_V5, sizeof(magic)) == 0;
    const bool v4 = memcmp(magic, ATP_SNAPSHOT_MAGIC_V4, sizeof(magic)) == 0;
    if (!v6 && !v5 && !v4) {
        fclose(file);
        return ATP_ERR_FORMAT;
    }

    const uint32_t version = atp_load_u32le(version_bytes);
    if (v4 || v5) {
        const uint32_t expected =
            v4 ? ATPERSON_SNAPSHOT_VERSION_V4 : ATPERSON_SNAPSHOT_VERSION_V5;
        if (version != expected || size < 8u + 4u + 12u + 8u) {
            fclose(file);
            return ATP_ERR_FORMAT;
        }
        /* v4/v5 carry no descriptor; they load at the legacy architecture. */
        fclose(file);
        *out_architecture = atp_neural_legacy_architecture();
        return ATP_OK;
    }
    if (version != ATPERSON_SNAPSHOT_VERSION || size < 8u + 4u + 12u + 8u) {
        fclose(file);
        return ATP_ERR_FORMAT;
    }

    size_t position = 12u;
    atp_neural_architecture architecture = {0};
    for (unsigned iteration = 0u; iteration < 128u; ++iteration) {
        if (size - position < 12u) {
            break;
        }
        unsigned char framing[12];
        if (fread(framing, 1u, sizeof(framing), file) != sizeof(framing)) {
            fclose(file);
            return ATP_ERR_IO;
        }
        position += 12u;
        const uint32_t tag = atp_load_u32le(framing);
        const uint64_t length = atp_load_u64le(framing + 4u);
        if (length > (uint64_t)(size - position)) {
            fclose(file);
            return ATP_ERR_FORMAT;
        }
        position += (size_t)length;
        if (tag != ATP_SECTION_ARCH) {
            if (length != 0u && fseek(file, (long)length, SEEK_CUR) != 0) {
                fclose(file);
                return ATP_ERR_FORMAT;
            }
            continue;
        }

        /* ARCH payload: version, embedding_dim, input_dim, output_dim,
         * hidden_layer_count, then ATPERSON_NEURAL_MAX_HIDDEN_LAYERS widths —
         * nine u32le fields (36 bytes), the exact shape atp_encode_arch
         * writes and atp_decode_arch reads. */
        if (length != (uint64_t)(5u + ATPERSON_NEURAL_MAX_HIDDEN_LAYERS) * 4u) {
            fclose(file);
            return ATP_ERR_FORMAT;
        }
        unsigned char payload[36];
        if (fread(payload, 1u, sizeof(payload), file) != sizeof(payload)) {
            fclose(file);
            return ATP_ERR_IO;
        }
        unsigned cursor = 0u;
        const unsigned char *at = payload;
        architecture.version = atp_load_u32le(at + cursor);
        cursor += 4u;
        architecture.embedding_dim = atp_load_u32le(at + cursor);
        cursor += 4u;
        architecture.input_dim = atp_load_u32le(at + cursor);
        cursor += 4u;
        architecture.output_dim = atp_load_u32le(at + cursor);
        cursor += 4u;
        architecture.hidden_layer_count = atp_load_u32le(at + cursor);
        cursor += 4u;
        for (size_t i = 0u; i < ATPERSON_NEURAL_MAX_HIDDEN_LAYERS; ++i) {
            architecture.hidden_widths[i] = atp_load_u32le(at + cursor);
            cursor += 4u;
        }

        atp_neural_layout layout = {0};
        if (!atp_neural_layout_build(&architecture, &layout)) {
            fclose(file);
            return ATP_ERR_FORMAT;
        }
        fclose(file);
        *out_architecture = architecture;
        return ATP_OK;
    }

    fclose(file);
    return ATP_ERR_FORMAT;
}