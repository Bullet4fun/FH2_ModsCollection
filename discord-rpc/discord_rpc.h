#ifndef FH2_DISCORD_RPC_H
#define FH2_DISCORD_RPC_H

#include "extra_options.h"

void discord_rpc_start(const char *client_id, const char *game_name); // a Discord application ID, a plain-ASCII name
void discord_rpc_stop(void);
void discord_rpc_update_activity(const char *state, const char *details);

#endif
