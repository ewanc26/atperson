#ifndef ATPERSON_AUDIT_TRANSPORT_HPP
#define ATPERSON_AUDIT_TRANSPORT_HPP

// Advisory audit: TypeSafe HTTP transport.
//
// Owns the single outbound POST to the TypeSafe System One endpoint using
// Wolfram's generic `wf_http_post` transport (libcurl backed). Network-only:
// this atom is compiled only in the ATPERSON_BUILD_NETWORK configuration and
// must never be pulled into the offline build. The API key is used solely for
// the `Authorization: Bearer` header and is never logged or persisted.
//
// Failure semantics: throws std::runtime_error when Wolfram cannot issue the
// request or the status is not a 2xx; a non-2xx response body is included in
// the message (401/422/429/529 hint at the likely cause).

#include <string>

namespace atperson {
namespace audit {

/**
 * POST `payload` to the TypeSafe System One endpoint.
 * `endpoint` is the full URL; `api_key` is the raw key (no "Bearer" prefix).
 * Returns the response body on a 2xx status.
 */
std::string post_typesafe_evaluation(const std::string &endpoint, const std::string &api_key,
                                     const std::string &payload);

} // namespace audit
} // namespace atperson

#endif