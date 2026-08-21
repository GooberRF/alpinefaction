#include "afstats_client.h"
#include "afstats_events.h"
#include "demo_upload.h"
#include "fflink.h"
#include "fflink_session.h"
#include "fflink_utils.h"

namespace fflink {

void do_patch()
{
    session_do_patch();
    afstats_client_do_patch();  // client-side: PSSK handshake + afstats_status cmd
    afstats::do_patch();        // server-side: event sender
    demo_upload_do_patch();     // server-side: demo upload queue
}

void do_frame()
{
    drain_pending_main_thread_tasks();
    drain_pending_console();
    afstats::do_frame();
    demo_upload_do_frame();     // server-side: demo upload pump
}

} // namespace fflink
