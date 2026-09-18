#ifndef ATPERSON_PERSISTENCE_FORMAT_H
#define ATPERSON_PERSISTENCE_FORMAT_H

/*
 * Snapshot wire-format identifiers shared by the persistence atoms.
 * This header owns identifiers only; changing these values is a format change,
 * not a refactor.
 *
 * v6 carries the explicit neural architecture descriptor and variable-length
 * network/node payloads (issue #72). v5 remains the legacy fragment format
 * with its historical one-hidden-layer wire shape; both load as current
 * graphs, v5 at the named legacy architecture.
 */
#define ATP_SNAPSHOT_MAGIC "ATPERSN5"
#define ATP_SNAPSHOT_MAGIC_V4 "ATPERSN1"
#define ATP_SNAPSHOT_MAGIC_V6 "ATPERSN6"
#define ATP_SNAPSHOT_VERSION_V4 4u

#define ATP_SECTION_HEADER 1u
#define ATP_SECTION_NETWORK 2u
#define ATP_SECTION_NODES 3u
#define ATP_SECTION_EDGES 4u
#define ATP_SECTION_LEDGER 5u
#define ATP_SECTION_EPISODES 6u
#define ATP_SECTION_FAMILIARITY 7u
#define ATP_SECTION_SCHEMA 8u
#define ATP_SECTION_VALENCE 9u
#define ATP_SECTION_CONTEXT 10u
/* v6: explicit neural architecture descriptor (issue #72). */
#define ATP_SECTION_ARCH 11u

#endif
