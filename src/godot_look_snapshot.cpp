#include "godot_look_snapshot.h"

#if defined(GODOT)

namespace godot_backend
{

namespace
{
LookSnapshot g_look_snapshot;
} // namespace

LookSnapshot &get_look_snapshot()
{
    return g_look_snapshot;
}

bool LookSnapshot::active() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_;
}

bool LookSnapshot::is_window( const void *window_id ) const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return active_ && window_id != nullptr && window_id == window_id_;
}

void LookSnapshot::publish( const void *window_id )
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = true;
    window_id_ = window_id;
}

void LookSnapshot::clear()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    active_ = false;
    window_id_ = nullptr;
}

} // namespace godot_backend

#endif // GODOT
