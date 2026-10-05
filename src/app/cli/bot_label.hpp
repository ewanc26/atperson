#pragma once

#include <cJSON.h>
#include <ostream>

namespace atperson::cli {

/* Inspect or explicitly set the Bluesky bot self-label on app.bsky.actor.profile.
 * This is operator-run setup only; the autonomous scheduler never calls it. */
int run_bot_label(std::ostream &out, std::ostream &err, const char *subcommand);

/* Pure profile-record helpers, kept separate so label mutation is testable
 * without credentials or network access. */
bool profile_has_bot_label(const cJSON *profile);
bool set_bot_self_label(cJSON *profile, bool enabled);

} // namespace atperson::cli
