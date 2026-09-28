#include "cbor_record.hpp"

#include "wolfram/repo/cbor.h"

#include <cJSON.h>

#include <cstdint>
#include <string>

namespace atperson {

namespace {

/* Hard cap on the nested structure the JSON conversion will walk. DAG-CBOR
 * record payloads are shallow (type, text, langs, reply, embed, facets), so
 * this bounds the walk rather than trusting payload depth. A field past the
 * cap is skipped, not crashed through. */
inline constexpr int kMaxCborDepth = 24;

/* Doubles represent integers exactly only through 2^53 - 1. Beyond that an
 * integer would silently change value, so it is emitted as a string. */
inline constexpr std::uint64_t kExactDoubleIntegers = (1ull << 53) - 1u;

const char *map_key_string(const wf_cbor_item *key) {
    if (key == nullptr || key->type != WF_CBOR_STRING) {
        return nullptr;
    }
    return key->string.str;
}

/* Emit an integer as a JSON number when the double conversion is exact,
 * otherwise as a JSON string so the value never silently changes. */
cJSON *integer_to_json(const wf_cbor_item *item) {
    if (item->type == WF_CBOR_UNSIGNED) {
        const std::uint64_t value = item->uinteger;
        if (value <= kExactDoubleIntegers) {
            return cJSON_CreateNumber(static_cast<double>(value));
        }
        const std::string text = std::to_string(value);
        return cJSON_CreateString(text.c_str());
    }
    /* Wolfram stores the CBOR negative magnitude; the represented value is
     * -1 - magnitude, so the value's magnitude is magnitude + 1. */
    const std::uint64_t magnitude = item->neginteger;
    if (magnitude < kExactDoubleIntegers) {
        return cJSON_CreateNumber(-1.0 - static_cast<double>(magnitude));
    }
    const std::string text = "-" + std::to_string(magnitude + 1u);
    return cJSON_CreateString(text.c_str());
}

cJSON *to_json(const wf_cbor_item *item, int depth) {
    if (item == nullptr || depth > kMaxCborDepth) {
        return nullptr;
    }
    switch (item->type) {
    case WF_CBOR_STRING:
        return cJSON_CreateString(item->string.str);
    case WF_CBOR_UNSIGNED:
    case WF_CBOR_NEGATIVE:
        return integer_to_json(item);
    case WF_CBOR_ARRAY: {
        cJSON *array = cJSON_CreateArray();
        if (array == nullptr) {
            return nullptr;
        }
        for (std::size_t i = 0u; i < item->children.count; ++i) {
            cJSON *element = to_json(item->children.items[i], depth + 1);
            if (element == nullptr) {
                continue;
            }
            if (!cJSON_AddItemToArray(array, element)) {
                cJSON_Delete(element);
            }
        }
        return array;
    }
    case WF_CBOR_MAP: {
        cJSON *object = cJSON_CreateObject();
        if (object == nullptr) {
            return nullptr;
        }
        for (std::size_t i = 0u; i < item->map.count; ++i) {
            const char *key = map_key_string(item->map.pairs[i].key);
            if (key == nullptr) {
                cJSON_Delete(object);
                return nullptr;
            }
            cJSON *value = to_json(item->map.pairs[i].value, depth + 1);
            if (value == nullptr) {
                continue;
            }
            if (!cJSON_AddItemToObject(object, key, value)) {
                cJSON_Delete(value);
            }
        }
        return object;
    }
    case WF_CBOR_SIMPLE:
        if (item->simple_value == 20) {
            return cJSON_CreateFalse();
        }
        if (item->simple_value == 21) {
            return cJSON_CreateTrue();
        }
        return cJSON_CreateNull();
    case WF_CBOR_BYTES:
    case WF_CBOR_LINK:
        /* Byte strings and CID links have no JSON form. The record
         * extractor only reads `uri` out of a strongRef, never `cid`, so
         * skipping the link costs the learning path nothing — and skipping
         * the field instead of failing the record is what keeps a reply
         * with a strongRef from being dropped whole. */
        return nullptr;
    }
    return nullptr;
}

} // namespace

cJSON *decode_cbor_record(const unsigned char *data, std::size_t len) {
    if (data == nullptr || len == 0u) {
        return nullptr;
    }
    wf_cbor_item *item = wf_cbor_parse(data, len);
    if (item == nullptr || item->type != WF_CBOR_MAP) {
        wf_cbor_free(item);
        return nullptr;
    }
    cJSON *record = to_json(item, 0);
    wf_cbor_free(item);
    return record;
}

} // namespace atperson