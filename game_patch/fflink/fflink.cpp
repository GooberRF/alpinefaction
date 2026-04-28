#include "fflink.h"
#include "fflink_session.h"
#include "fflink_utils.h"

namespace fflink {

void do_patch()
{
    session_do_patch();
}

void do_frame()
{
    drain_pending_console();
}

} // namespace fflink
