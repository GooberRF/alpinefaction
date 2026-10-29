#pragma once

namespace rf
{
    struct NetAddr;
}

void discord_presence_init();
void discord_presence_do_frame();
void discord_presence_shutdown();
void discord_presence_on_join_request(const rf::NetAddr& addr, bool has_password);
