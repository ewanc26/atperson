#ifndef ATPERSON_ATPROTO_CBOR_RECORD_HPP
#define ATPERSON_ATPROTO_CBOR_RECORD_HPP

#include <cstddef>

struct cJSON;

namespace atperson {

/* Decode one DAG-CBOR record payload into the cJSON object shape the record
 * extractor consumes (the same `record` subtree the live Jetstream JSON path
 * produces). This owns the CBOR -> JSON conversion for the Jetstream archive
 * path; wolfram owns the CBOR decoding primitive.
 *
 * Ownership: the caller owns and frees the returned cJSON tree with
 * cJSON_Delete. Returns nullptr when the payload is not a decodable
 * canonical DAG-CBOR map (malformed bytes, trailing bytes, or a map key that
 * is not a string, which means the bytes are not a normal record) — the
 * caller treats that as a dropped row, never as learned text. Within a record
 * that does decode, a field whose value is not representable as JSON (byte
 * strings, CID links) or that recurses past the bounded depth is skipped
 * rather than failing the whole record. */
[[nodiscard]] cJSON *decode_cbor_record(const unsigned char *data, std::size_t len);

} // namespace atperson

#endif