#include "godot_anim_snapshot.h"

#if defined(GODOT)

#include "color.h"
#include "godot_backend.h"

#include <godot_cpp/variant/vector3i.hpp>

namespace godot_backend
{

namespace
{

/// Pack an RGBA colour the way anim_cmd::fg / ::bg expect.
int32_t pack_rgba( const color &c )
{
    return static_cast<int32_t>( ( static_cast<uint32_t>( c.r ) << 24 ) |
                                 ( static_cast<uint32_t>( c.g ) << 16 ) |
                                 ( static_cast<uint32_t>( c.b ) << 8 ) |
                                 static_cast<uint32_t>( c.a ) );
}

} // namespace

void AnimSnapshot::add_glyph( const tripoint_bub_ms &p, const char32_t ch,
                              const nc_color &color )
{
    anim_cmd cmd;
    cmd.kind = static_cast<int32_t>( anim_kind::glyph );
    cmd.x = p.x();
    cmd.y = p.y();
    cmd.z = p.z();
    cmd.codepoint = static_cast<int32_t>( ch );
    // curses_color_to_color resolves a colour pair to its foreground, falling back
    // to the background, so nc_colors built by red_background() and friends come
    // through as their highlight colour rather than as a background fill.
    cmd.fg = pack_rgba( curses_color_to_color( color ) );
    cmd.bg = 0;

    std::lock_guard<std::mutex> lock( mutex_ );
    ++glyphs_added_;
    pending_.push_back( cmd );
}

void AnimSnapshot::add_hit( const tripoint_bub_ms &p, const tripoint_bub_ms &from,
                            const nc_color &color )
{
    /// More than this and the oldest is dropped; see AnimSnapshot::hits_.
    constexpr size_t max_recent_hits = 32;

    anim_hit hit;
    hit.x = p.x();
    hit.y = p.y();
    hit.z = p.z();
    // Sign only: the reaction is a nudge in the direction of the blow, not a
    // displacement proportional to how far away the attacker stood.
    hit.dir_x = ( p.x() > from.x() ) - ( p.x() < from.x() );
    hit.dir_y = ( p.y() > from.y() ) - ( p.y() < from.y() );
    hit.flash = pack_rgba( curses_color_to_color( color ) );

    std::lock_guard<std::mutex> lock( mutex_ );
    hit.id = static_cast<int32_t>( ++hit_seq_ );
    hits_.push_back( hit );
    if( hits_.size() > max_recent_hits ) {
        hits_.erase( hits_.begin(), hits_.end() - max_recent_hits );
    }
}

godot::PackedInt32Array AnimSnapshot::copy_hits() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::PackedInt32Array out;
    out.resize( static_cast<int64_t>( hits_.size() * hit_stride ) );
    int32_t *dst = out.ptrw();
    size_t i = 0;
    for( const anim_hit &h : hits_ ) {
        dst[i++] = h.id;
        dst[i++] = h.x;
        dst[i++] = h.y;
        dst[i++] = h.z;
        dst[i++] = h.dir_x;
        dst[i++] = h.dir_y;
        dst[i++] = h.flash;
    }
    return out;
}

uint64_t AnimSnapshot::hit_generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return hit_seq_;
}

void AnimSnapshot::add_highlight( const tripoint_bub_ms &p )
{
    anim_cmd cmd;
    cmd.kind = static_cast<int32_t>( anim_kind::highlight );
    cmd.x = p.x();
    cmd.y = p.y();
    cmd.z = p.z();

    std::lock_guard<std::mutex> lock( mutex_ );
    pending_.push_back( cmd );
}

void AnimSnapshot::add_text( const tripoint_bub_ms &p, const std::string &text,
                             const nc_color &color, const anim_align align, const float life,
                             const int run )
{
    if( text.empty() ) {
        return;
    }
    anim_text t;
    t.x = p.x();
    t.y = p.y();
    t.z = p.z();
    t.text = text;
    t.fg = pack_rgba( curses_color_to_color( color ) );
    t.align = align;
    t.life = life;
    t.run = run;

    std::lock_guard<std::mutex> lock( mutex_ );
    ++texts_added_;
    pending_texts_.push_back( t );
}

godot::Array AnimSnapshot::copy_texts() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Array out;
    for( const anim_text &t : published_texts_ ) {
        godot::Dictionary d;
        d["pos"] = godot::Vector3i( t.x, t.y, t.z );
        d["text"] = godot::String::utf8( t.text.c_str() );
        d["fg"] = t.fg;
        d["align"] = static_cast<int>( t.align );
        d["life"] = t.life;
        d["run"] = t.run;
        out.push_back( d );
    }
    return out;
}

godot::Dictionary AnimSnapshot::copy_stats() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary out;
    out["commits"] = static_cast<int64_t>( commits_ );
    out["glyphs_added"] = static_cast<int64_t>( glyphs_added_ );
    out["texts_added"] = static_cast<int64_t>( texts_added_ );
    out["hits_added"] = static_cast<int64_t>( hit_seq_ );
    out["generation"] = static_cast<int64_t>( generation_ );
    return out;
}

void AnimSnapshot::commit_frame()
{
    std::lock_guard<std::mutex> lock( mutex_ );
    ++commits_;
    if( pending_.empty() && published_.empty() &&
        pending_texts_.empty() && published_texts_.empty() ) {
        // Idle: no animation running and nothing left over. Do not bump the
        // generation, so the Godot side keeps skipping the overlay entirely.
        return;
    }
    published_ = std::move( pending_ );
    pending_.clear();
    published_texts_ = std::move( pending_texts_ );
    pending_texts_.clear();
    ++generation_;
}

godot::PackedInt32Array AnimSnapshot::copy_commands() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::PackedInt32Array out;
    out.resize( static_cast<int64_t>( published_.size() * cmd_stride ) );
    int32_t *dst = out.ptrw();
    size_t i = 0;
    for( const anim_cmd &c : published_ ) {
        dst[i++] = c.kind;
        dst[i++] = c.x;
        dst[i++] = c.y;
        dst[i++] = c.z;
        dst[i++] = c.codepoint;
        dst[i++] = c.fg;
        dst[i++] = c.bg;
    }
    return out;
}

int AnimSnapshot::command_count() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return static_cast<int>( published_.size() );
}

uint64_t AnimSnapshot::generation() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    return generation_;
}

AnimSnapshot &get_anim_snapshot()
{
    static AnimSnapshot instance;
    return instance;
}

} // namespace godot_backend

#endif // GODOT
