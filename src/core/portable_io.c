#include "portable_io.h"

#include <string.h>

/*
 * Portable binary IO primitives. See portable_io.h for the format contract.
 *
 * Floats: C23 does not guarantee IEEE 754, but every platform this project
 * targets (CI matrix: x86_64/arm64 GCC and Clang) uses IEEE 754 floats, and
 * the static asserts in the snapshot reader refuse to load on a host where
 * the bit patterns would not round-trip. The memcpy-based bit casts avoid
 * strict-aliasing and trap-representation pitfalls.
 */

_Static_assert(sizeof(float) == 4u, "snapshot format requires 32-bit float");
_Static_assert(sizeof(double) == 8u, "snapshot format requires 64-bit double");

void atp_store_u32le(unsigned char *out, uint32_t value) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
    out[2] = (unsigned char)((value >> 16) & 0xffu);
    out[3] = (unsigned char)((value >> 24) & 0xffu);
}

void atp_store_u64le(unsigned char *out, uint64_t value) {
    for (unsigned i = 0u; i < 8u; ++i) {
        out[i] = (unsigned char)((value >> (8u * i)) & 0xffu);
    }
}

uint32_t atp_load_u32le(const unsigned char *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

uint64_t atp_load_u64le(const unsigned char *in) {
    uint64_t value = 0u;
    for (unsigned i = 0u; i < 8u; ++i) {
        value |= (uint64_t)in[i] << (8u * i);
    }
    return value;
}

void atp_store_f32le(unsigned char *out, float value) {
    uint32_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    atp_store_u32le(out, bits);
}

void atp_store_f64le(unsigned char *out, double value) {
    uint64_t bits = 0u;
    memcpy(&bits, &value, sizeof(bits));
    atp_store_u64le(out, bits);
}

float atp_load_f32le(const unsigned char *in) {
    const uint32_t bits = atp_load_u32le(in);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

double atp_load_f64le(const unsigned char *in) {
    const uint64_t bits = atp_load_u64le(in);
    double value = 0.0;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

uint64_t atp_fnv1a64(const void *data, size_t length) {
    const unsigned char *cursor = data;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0u; i < length; ++i) {
        hash ^= cursor[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void atp_store_section(unsigned char *out, uint32_t tag, uint64_t length) {
    atp_store_u32le(out, tag);
    atp_store_u64le(out + 4u, length);
}

bool atp_load_section(const unsigned char *in, size_t available, atp_section *out) {
    if (available < 12u) {
        return false;
    }
    out->tag = atp_load_u32le(in);
    out->length = atp_load_u64le(in + 4u);
    return true;
}
