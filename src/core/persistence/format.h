#ifndef ATPERSON_PERSISTENCE_FORMAT_H
#define ATPERSON_PERSISTENCE_FORMAT_H

/*
 * Snapshot wire-format identifiers shared by the persistence atoms.
 * This header owns identifiers only; changing these values is a format change,
 * not a refactor.
 */
#define ATP_SNAPSHOT_MAGIC "ATPERSN5"
#define ATP_SNAPSHOT_MAGIC_V4 "ATPERSN1"

#define ATP_SECTION_HEADER 1u
#define ATP_SECTION_NETWORK 2u
#define ATP_SECTION_NODES 3u
#define ATP_SECTION_EDGES 4u
#define ATP_SECTION_LEDGER 5u
#define ATP_SECTION_EPISODES 6u
#define ATP_SECTION_FAMILIARITY 7u
#define ATP_SECTION_SCHEMA 8u
#define ATP_SECTION_VALENCE 9u

#endif
