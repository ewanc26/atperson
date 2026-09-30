#include "cbor_record.hpp"

#include "wolfram/repo/cbor.h"

#include <cJSON.h>

#include <cstdint>
#include <string>
#include <vector>

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

/* Deepest container nesting the pre-scan admits. Real records nest a handful
 * of levels (facets, embeds); this only exists to bound hostile input. */
inline constexpr std::size_t kMaxScanDepth = 64;

/* Cheap, iterative structural check run BEFORE the real parser sees untrusted
 * bytes. It does not decode anything: it only refuses inputs whose declared
 * shape cannot possibly be backed by the bytes present, or that nest deeper
 * than any record does. Without it a payload of a few bytes declaring an array
 * of 2^32-1 children makes the parser allocate and grind for many seconds.
 *
 * Every array child and every string/byte byte occupies at least one byte and
 * every map pair at least two, so a declared count larger than the remaining
 * input is impossible and rejected. Indefinite lengths and reserved additional
 * information are not valid DAG-CBOR and are rejected here too. Runs in O(n)
 * with an explicit stack, so it cannot itself be recursed or amplified. */
bool structurally_bounded(const unsigned char *data, std::size_t len) {
    std::size_t pos = 0;
    std::vector<std::uint64_t> remaining{1u}; /* items still owed per open container */
    while (!remaining.empty()) {
        if (remaining.back() == 0u) {
            remaining.pop_back();
            continue;
        }
        remaining.back()--;
        if (pos >= len) {
            return false;
        }
        const unsigned char initial = data[pos++];
        const unsigned major = initial >> 5u;
        const unsigned info = initial & 0x1Fu;
        std::uint64_t argument = info;
        if (info >= 28u) {
            return false; /* reserved, or indefinite length */
        }
        if (info >= 24u) {
            const std::size_t width = std::size_t{1} << (info - 24u);
            if (len - pos < width) {
                return false;
            }
            argument = 0u;
            for (std::size_t i = 0; i < width; ++i) {
                argument = (argument << 8u) | data[pos++];
            }
        }
        const std::uint64_t left = len - pos;
        switch (major) {
        case 2: /* byte string */
        case 3: /* text string */
            if (argument > left) {
                return false;
            }
            pos += static_cast<std::size_t>(argument);
            break;
        case 4: /* array */
            if (argument > left) {
                return false;
            }
            if (argument > 0u) {
                remaining.push_back(argument);
            }
            break;
        case 5: /* map */
            if (argument > left / 2u) {
                return false;
            }
            if (argument > 0u) {
                remaining.push_back(argument * 2u);
            }
            break;
        case 6: /* tag: wraps exactly one item */
            remaining.push_back(1u);
            break;
        default: /* 0, 1 integers; 7 simple/float: fully consumed above */
            break;
        }
        if (remaining.size() > kMaxScanDepth) {
            return false;
        }
    }
    return pos == len;
}

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
    if (!structurally_bounded(data, len)) {
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