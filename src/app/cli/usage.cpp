#include "cli/usage.hpp"

#include <ostream>

namespace atperson {
namespace cli {

void print_usage(std::ostream &out) {
    out << "usage:\n"
        << "  atperson stats\n"
        << "  atperson resources\n"
        << "  atperson ingest <text> [source-id]\n"
        << "  atperson ingest-file <path> [source-id]\n"
        << "  atperson assoc <token> [limit]\n"
        << "  atperson candidates <context> [limit]\n"
        << "  atperson plans <context> [max-tokens] [beam-width]\n"
        << "  atperson decide <context> [max-tokens] [beam-width]\n"
        << "  atperson audit <context> [max-tokens] [beam-width]\n"
        << "  atperson context <text> [source-id] [author-did]\n"
        << "  atperson familiarity <token>\n"
        << "  atperson recall <query> [limit]\n"
        << "  atperson sync [max-pages]\n"
        << "  atperson rebuild\n"
        << "  atperson compact\n"
        << "  atperson withdraw <id|source|author> <target>\n"
        << "  atperson cursor [status|reset]\n"
        << "  atperson control <status|pause|resume|writes <on|off>|dry-run <on|off>|"
           "approval <on|off>|approve <digest>|revoke <digest>|shutdown|cancel-shutdown>\n\n"
        << "environment:\n"
        << "  ATPERSON_STATE            model snapshot path "
           "(default ~/.ewanc26/atperson/model.bin)\n"
        << "  ATPERSON_LEDGER           observation ledger path "
           "(default ~/.ewanc26/atperson/ledger.bin)\n"
        << "  ATPERSON_INGESTION_STATE  ingestion cursor path "
           "(default ~/.ewanc26/atperson/ingestion-state.json)\n"
        << "  ATPERSON_CONTROL_STATE    operator control path "
           "(default ~/.ewanc26/atperson/control-state.json)\n"
        << "  ATPERSON_HOME             data directory override "
           "(default ~/.ewanc26/atperson)\n"
        << "  ATPERSON_MEMORY_BUDGET_BYTES   graph growth memory override (default auto)\n"
        << "  ATPERSON_DISK_RESERVE_BYTES    free-space reserve override (default auto)\n"
        << "  ATPERSON_NODE_CAPACITY         graph node ceiling override (default auto)\n"
        << "  ATPERSON_EDGE_CAPACITY         graph edge ceiling override (default auto)\n"
        << "  ATPERSON_SYNC_PAGE_SIZE        items per timeline page (default auto)\n"
        << "  ATPERSON_SYNC_MAX_OBSERVATIONS per-run sync observation budget (default auto)\n"
        << "  ATPERSON_SERVICE       PDS/service URL (default https://bsky.social)\n"
        << "  ATPERSON_IDENTIFIER    handle or email for sync\n"
        << "  ATPERSON_APP_PASSWORD  app password for sync\n\n"
        << "  ATPERSON_TYPESAFE_API_KEY   opt-in key for the advisory `audit` command\n"
        << "  ATPERSON_TYPESAFE_ENDPOINT  advisory audit endpoint "
           "(default https://api.typesafe.ai/v1/systemone)\n\n"
        << "mutating commands take an exclusive lock on the data directory;\n"
        << "read-only commands run without it and see state as of their read\n";
}

} // namespace cli
} // namespace atperson