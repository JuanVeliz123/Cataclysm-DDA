#include "godot_hud_snapshot.h"

#if defined(GODOT)

#include "avatar.h"
#include "bionics.h"
#include "character.h"
#include "creature.h"
#include "display.h"
#include "game.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "item_category.h"
#include "itype.h"
#include "messages.h"
#include "mutation.h"
#include "output.h"
#include "skill.h"
#include "translations.h"
#include "character_martial_arts.h"
#include "effect.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "units_utility.h"
#include "weather.h"

#include <algorithm>
#include <array>
#include <map>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot_backend
{

namespace
{

HudSnapshot g_hud_snapshot;

/// std::string -> godot::String.
///
/// Must go through String::utf8: the String(const char *) constructor decodes
/// latin-1, so every accented character, box glyph and non-breaking space in an
/// item or effect name arrived mangled ("++\u00c2 rubber gloves").
godot::String gs( const std::string &s )
{
    return godot::String::utf8( s.c_str() );
}

std::string plain( const std::string &s )
{
    return remove_color_tags( s );
}

/// Collapse a game_message_type onto the design's three status tones. Nocturne
/// is monochrome and introduces exactly three hues; everything else reads "dim".
std::string tone_for( const game_message_type type )
{
    switch( type ) {
        case m_bad:
            return "bad";
        case m_warning:
        case m_mixed:
            return "warn";
        default:
            return "dim";
    }
}

godot::Array kv_array( const std::vector<HudSnapshot::kv> &rows )
{
    godot::Array out;
    out.resize( static_cast<int64_t>( rows.size() ) );
    for( size_t i = 0; i < rows.size(); ++i ) {
        godot::Dictionary d;
        d["name"] = gs( rows[i].name );
        d["detail"] = gs( rows[i].detail );
        out[static_cast<int64_t>( i )] = d;
    }
    return out;
}

} // namespace

HudSnapshot &get_hud_snapshot()
{
    return g_hud_snapshot;
}

void update_hud_snapshot()
{
    get_hud_snapshot().update_from_game();
}

void HudSnapshot::update_from_game()
{
    if( !g ) {
        return;
    }
    avatar &u = get_avatar();

    hud_data hud;
    character_data sheet;
    inventory_data inv;

    hud.name = u.get_name();
    sheet.name = hud.name;
    hud.date = display::date_string();
    hud.time = plain( display::time_string( u ) );
    hud.location = plain( display::current_position_text( u.pos_abs_omt() ) );
    hud.weather = plain( display::weather_text_color( u ).first );
    hud.hunger = display::hunger_text_color( u ).first;
    hud.thirst = display::thirst_text_color( u ).first;
    hud.sleepiness = display::sleepiness_text_color( u ).first;
    hud.pain = display::pain_text_color( u ).first;
    hud.move_mode = display::move_mode_letter_color( u ).first;
    hud.carry = plain( display::carry_weight_value_color( u ).first );
    inv.carry = hud.carry;
    // volume_carried() is the outer bulk of the wielded and worn items, while
    // volume_capacity() is how much their pockets hold -- not a used/total pair,
    // and dividing one by the other produced readings like "5.34 / 0.75 L".
    // Pocket space used is capacity minus what is still free.
    const units::volume vol_cap = u.volume_capacity();
    const units::volume vol_used = vol_cap - u.free_space();
    inv.volume = plain( format_volume( vol_used ) + " / " +
                        format_volume( vol_cap ) + " " + volume_units_abbr() );
    // The design draws both capacities as segmented gauges, which need a ratio
    // rather than the formatted string.
    const units::mass weight_cap = u.weight_capacity();
    inv.carry_pct = weight_cap > 0_gram
                    ? static_cast<int>( u.weight_carried() * 100 / weight_cap )
                    : 0;
    inv.volume_pct = vol_cap > 0_ml
                     ? static_cast<int>( vol_used * 100 / vol_cap )
                     : 0;

    // Sidebar v2 "01 Vitals": everything the design shows beside the bars.
    hud.temperature = plain( display::temp_text_color( u ).first );
    if( item_location wielded = u.get_wielded_item() ) {
        if( const item *it = wielded.get_item() ) {
            hud.weapon = plain( it->display_name() );
            hud.weapon_glyph = plain( it->symbol() );
        }
    }
    if( hud.weapon.empty() ) {
        hud.weapon = _( "fists" );
        hud.weapon_glyph = "@";
    }
    hud.style = plain( u.martial_arts_data->selected_style_name( u ) );

    // The design's four-cell metrics grid, and the values its bars need as
    // numbers rather than as the pre-formatted strings above.
    hud.focus = u.get_focus();
    hud.speed = u.get_speed();
    hud.sound = u.volume;
    hud.pain_level = u.get_perceived_pain();
    hud.morale_level = u.get_morale_level();
    const units::energy max_power = u.get_max_power_level();
    hud.power_pct = max_power > 0_kJ
                    ? static_cast<int>( u.get_power_level() * 100 / max_power )
                    : -1;

    for( const std::reference_wrapper<const effect> &eff : u.get_effects() ) {
        const std::string desc = plain( eff.get().disp_name() );
        if( desc.empty() ) {
            continue;
        }
        // The effect type already rates itself for the message log; reuse that
        // rather than inventing a second opinion about which effects are bad.
        hud.effects.push_back( { desc,
                                 tone_for( eff.get().get_effect_type()->get_rating(
                                         eff.get().get_intensity() ) ) } );
    }
    // Safe mode is a chip in the design rather than a field of its own, and it is
    // only worth a chip when it is off -- that is the state you need telling about.
    if( g->safe_mode == SAFE_MODE_OFF ) {
        hud.effects.push_back( { _( "Safe mode off" ), "warn" } );
    }

    // "02 Contacts" and the 3x3 compass, from the creatures the player can
    // actually see. An earlier cut read monster_visible_info::unique_mons, which
    // is aggregated per direction and carries no distance -- the design wants
    // "3 NE" per contact, so walk the creatures themselves.
    {
        // Design grid order: NW N NE / W . E / SW S SE.
        static const std::array<const char *, 8> dir_names = {{
                translate_marker( "NW" ), translate_marker( "N" ), translate_marker( "NE" ),
                translate_marker( "W" ), translate_marker( "E" ),
                translate_marker( "SW" ), translate_marker( "S" ), translate_marker( "SE" )
            }
        };
        hud.compass.assign( dir_names.size(), 0 );

        // Bucket into the eight compass points the grid draws. Doing the sign
        // test here rather than going through direction_from keeps the mapping
        // to a grid cell explicit, and drops the above/below cases the grid has
        // nowhere to put.
        const auto slot_for = []( const int dx, const int dy ) -> int {
            const int sx = dx > 0 ? 1 : ( dx < 0 ? -1 : 0 );
            const int sy = dy > 0 ? 1 : ( dy < 0 ? -1 : 0 );
            if( sy < 0 ) {
                return sx < 0 ? 0 : ( sx == 0 ? 1 : 2 );
            }
            if( sy == 0 ) {
                return sx < 0 ? 3 : ( sx == 0 ? -1 : 4 );
            }
            return sx < 0 ? 5 : ( sx == 0 ? 6 : 7 );
        };

        const tripoint_bub_ms here = u.pos_bub();
        int hostiles = 0;
        for( Creature *c : u.get_visible_creatures( MAX_VIEW_DISTANCE ) ) {
            if( !c || c == &u ) {
                continue;
            }
            const tripoint_bub_ms there = c->pos_bub();
            const int slot = slot_for( there.x() - here.x(), there.y() - here.y() );
            if( slot >= 0 ) {
                hud.compass[static_cast<size_t>( slot )] += 1;
            }
            const Creature::Attitude att = c->attitude_to( u );
            if( att == Creature::Attitude::HOSTILE ) {
                ++hostiles;
            }
            contact ct;
            ct.symbol = plain( c->symbol() );
            ct.name = plain( c->disp_name() );
            ct.meta = string_format( "%d %s", rl_dist( here, there ),
                                     slot >= 0 ? _( dir_names[slot] ) : _( "here" ) );
            ct.tone = att == Creature::Attitude::HOSTILE ? "bad"
                      : ( att == Creature::Attitude::FRIENDLY ? "dim" : "warn" );
            hud.contacts.push_back( std::move( ct ) );
        }
        // Nearest first: a contact list is read for what is about to reach you.
        std::sort( hud.contacts.begin(), hud.contacts.end(),
        []( const contact & a, const contact & b ) {
            return a.tone == b.tone ? a.name < b.name : a.tone < b.tone;
        } );
        hud.threat_summary = hostiles > 0
                             ? string_format( n_gettext( "%d hostile", "%d hostiles", hostiles ),
                                              hostiles )
                             : std::string( _( "clear" ) );
    }
    hud.hp = u.get_hp();
    hud.hp_max = u.get_hp_max();
    hud.stamina = u.get_stamina();
    hud.stamina_max = u.get_stamina_max();
    hud.str = u.get_str();
    hud.dex = u.get_dex();
    hud.intel = u.get_int();
    hud.per = u.get_per();
    sheet.str = hud.str;
    sheet.dex = hud.dex;
    sheet.intel = hud.intel;
    sheet.per = hud.per;

    for( const bodypart_id &bp : u.get_all_body_parts( get_body_part_flags::only_main ) ) {
        limb l;
        l.name = bp->name.translated();
        l.hp = u.get_hp( bp );
        l.hp_max = u.get_hp_max( bp );
        hud.limbs.push_back( l );
        sheet.limbs.push_back( l );
    }

    // "03 Log": the design keeps the timestamp in its own column and marks each
    // line with a tone, so hand over the parts rather than one joined string.
    // recent_messages gives (time, colourised text); the colour is the only
    // record of severity Messages keeps, so read the tone off it before stripping.
    const auto msgs = Messages::recent_messages( 12 );
    hud.messages.reserve( msgs.size() );
    for( const auto &m : msgs ) {
        log_line line;
        line.stamp = plain( m.first );
        line.text = plain( m.second );
        if( m.second.find( "_red" ) != std::string::npos ) {
            line.tone = "bad";
        } else if( m.second.find( "yellow" ) != std::string::npos ||
                   m.second.find( "_pink" ) != std::string::npos ) {
            line.tone = "warn";
        } else {
            line.tone = "dim";
        }
        hud.messages.push_back( std::move( line ) );
    }

    std::vector<const Skill *> skills = Skill::get_skills_sorted_by(
    []( const Skill & a, const Skill & b ) {
        if( a.get_sort_rank() != b.get_sort_rank() ) {
            return a.get_sort_rank() < b.get_sort_rank();
        }
        return a.name() < b.name();
    } );
    for( const Skill *sk : skills ) {
        if( !sk || sk->ident().is_empty() ) {
            continue;
        }
        const SkillLevel &lvl = u.get_skill_level_object( sk->ident() );
        if( lvl.level() <= 0 && lvl.knowledgeLevel() <= 0 ) {
            continue;
        }
        skill s;
        s.name = sk->name();
        s.level = lvl.level();
        s.knowledge = lvl.knowledgeLevel();
        sheet.skills.push_back( s );
    }

    for( const trait_id &tr : u.get_mutations( /*include_hidden=*/false ) ) {
        if( !tr.is_valid() ) {
            continue;
        }
        const mutation_branch &mut = tr.obj();
        if( !mut.player_display ) {
            continue;
        }
        kv row;
        row.name = mut.name();
        row.detail = mut.desc();
        sheet.traits.push_back( row );
    }

    for( const bionic_id &bid : u.get_bionics() ) {
        if( !bid.is_valid() ) {
            continue;
        }
        kv row;
        row.name = bid->name.translated();
        row.detail = bid->description.translated();
        sheet.bionics.push_back( row );
    }

    sheet.status.push_back( { _( "Hunger" ), hud.hunger } );
    sheet.status.push_back( { _( "Thirst" ), hud.thirst } );
    sheet.status.push_back( { _( "Sleepiness" ), hud.sleepiness } );
    sheet.status.push_back( { _( "Pain" ), hud.pain } );
    sheet.status.push_back( { _( "Weight" ), display::weight_text_color( u ).first } );

    auto add_item = [&]( const item & it, const char *where ) {
        inv_item row;
        row.where = where;
        row.name = plain( it.display_name() );
        row.uid = static_cast<int64_t>( it.uid().get_value() );
        // Worn items form their own group in the design rather than being filed
        // under whatever they would otherwise be categorised as.
        row.category = std::string( where ) == "worn"
                       ? std::string( _( "ITEMS WORN" ) )
                       : plain( it.get_category_shallow().name_header() );
        row.key = it.invlet > 0 ? std::string( 1, it.invlet ) : std::string();
        // Charges where there are any, otherwise the volume it takes up.
        row.meta = it.count_by_charges()
                   ? string_format( "%d", it.charges )
                   : plain( string_format( "%s %s", format_volume( it.volume() ),
                                           volume_units_abbr() ) );
        if( it.type ) {
            row.info = plain( it.type->description.translated() );
        }
        inv.items.push_back( std::move( row ) );
    };

    if( item_location wielded = u.get_wielded_item() ) {
        if( const item *it = wielded.get_item() ) {
            add_item( *it, "wielded" );
        }
    }
    for( const item_location &loc : u.get_visible_worn_items() ) {
        if( const item *it = loc.get_item() ) {
            add_item( *it, "worn" );
        }
    }
    for( const std::list<item> *stack : u.inv->const_slice() ) {
        if( stack == nullptr || stack->empty() ) {
            continue;
        }
        add_item( stack->front(), "carried" );
    }

    std::lock_guard<std::mutex> lock( mutex_ );
    hud_ = std::move( hud );
    character_ = std::move( sheet );
    inventory_ = std::move( inv );
}

godot::Dictionary HudSnapshot::copy_hud() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["name"] = gs( hud_.name );
    d["date"] = gs( hud_.date );
    d["time"] = gs( hud_.time );
    d["location"] = gs( hud_.location );
    d["weather"] = gs( hud_.weather );
    d["hunger"] = gs( hud_.hunger );
    d["thirst"] = gs( hud_.thirst );
    d["sleepiness"] = gs( hud_.sleepiness );
    d["pain"] = gs( hud_.pain );
    d["move_mode"] = gs( hud_.move_mode );
    d["carry"] = gs( hud_.carry );
    d["temperature"] = gs( hud_.temperature );
    d["weapon"] = gs( hud_.weapon );
    d["style"] = gs( hud_.style );
    d["threat_summary"] = gs( hud_.threat_summary );
    d["power_pct"] = hud_.power_pct;
    d["focus"] = hud_.focus;
    d["speed"] = hud_.speed;
    d["sound"] = hud_.sound;
    d["pain_level"] = hud_.pain_level;
    d["morale_level"] = hud_.morale_level;
    d["weapon_glyph"] = gs( hud_.weapon_glyph );
    godot::Array compass;
    compass.resize( static_cast<int64_t>( hud_.compass.size() ) );
    for( size_t i = 0; i < hud_.compass.size(); ++i ) {
        compass[static_cast<int64_t>( i )] = hud_.compass[i];
    }
    d["compass"] = compass;
    godot::Array effects;
    effects.resize( static_cast<int64_t>( hud_.effects.size() ) );
    for( size_t i = 0; i < hud_.effects.size(); ++i ) {
        godot::Dictionary e;
        e["label"] = gs( hud_.effects[i].label );
        e["tone"] = gs( hud_.effects[i].tone );
        effects[static_cast<int64_t>( i )] = e;
    }
    d["effects"] = effects;
    godot::Array contacts;
    contacts.resize( static_cast<int64_t>( hud_.contacts.size() ) );
    for( size_t i = 0; i < hud_.contacts.size(); ++i ) {
        godot::Dictionary c;
        c["symbol"] = gs( hud_.contacts[i].symbol );
        c["name"] = gs( hud_.contacts[i].name );
        c["meta"] = gs( hud_.contacts[i].meta );
        c["tone"] = gs( hud_.contacts[i].tone );
        contacts[static_cast<int64_t>( i )] = c;
    }
    d["contacts"] = contacts;
    d["hp"] = hud_.hp;
    d["hp_max"] = hud_.hp_max;
    d["stamina"] = hud_.stamina;
    d["stamina_max"] = hud_.stamina_max;
    d["str"] = hud_.str;
    d["dex"] = hud_.dex;
    d["int"] = hud_.intel;
    d["per"] = hud_.per;
    godot::Array msgs;
    msgs.resize( static_cast<int64_t>( hud_.messages.size() ) );
    for( size_t i = 0; i < hud_.messages.size(); ++i ) {
        godot::Dictionary m;
        m["stamp"] = gs( hud_.messages[i].stamp );
        m["text"] = gs( hud_.messages[i].text );
        m["tone"] = gs( hud_.messages[i].tone );
        msgs[static_cast<int64_t>( i )] = m;
    }
    d["messages"] = msgs;
    godot::Array limbs;
    limbs.resize( static_cast<int64_t>( hud_.limbs.size() ) );
    for( size_t i = 0; i < hud_.limbs.size(); ++i ) {
        godot::Dictionary l;
        l["name"] = gs( hud_.limbs[i].name );
        l["hp"] = hud_.limbs[i].hp;
        l["hp_max"] = hud_.limbs[i].hp_max;
        limbs[static_cast<int64_t>( i )] = l;
    }
    d["limbs"] = limbs;
    return d;
}

godot::Dictionary HudSnapshot::copy_character() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["name"] = gs( character_.name );
    d["str"] = character_.str;
    d["dex"] = character_.dex;
    d["int"] = character_.intel;
    d["per"] = character_.per;
    godot::Array limbs;
    limbs.resize( static_cast<int64_t>( character_.limbs.size() ) );
    for( size_t i = 0; i < character_.limbs.size(); ++i ) {
        godot::Dictionary l;
        l["name"] = gs( character_.limbs[i].name );
        l["hp"] = character_.limbs[i].hp;
        l["hp_max"] = character_.limbs[i].hp_max;
        limbs[static_cast<int64_t>( i )] = l;
    }
    d["limbs"] = limbs;
    godot::Array skills;
    skills.resize( static_cast<int64_t>( character_.skills.size() ) );
    for( size_t i = 0; i < character_.skills.size(); ++i ) {
        godot::Dictionary s;
        s["name"] = gs( character_.skills[i].name );
        s["level"] = character_.skills[i].level;
        s["knowledge"] = character_.skills[i].knowledge;
        skills[static_cast<int64_t>( i )] = s;
    }
    d["skills"] = skills;
    d["traits"] = kv_array( character_.traits );
    d["bionics"] = kv_array( character_.bionics );
    d["status"] = kv_array( character_.status );
    return d;
}

godot::Dictionary HudSnapshot::copy_inventory() const
{
    std::lock_guard<std::mutex> lock( mutex_ );
    godot::Dictionary d;
    d["carry"] = gs( inventory_.carry );
    d["volume"] = gs( inventory_.volume );
    d["carry_pct"] = inventory_.carry_pct;
    d["volume_pct"] = inventory_.volume_pct;
    godot::Array items;
    items.resize( static_cast<int64_t>( inventory_.items.size() ) );
    for( size_t i = 0; i < inventory_.items.size(); ++i ) {
        godot::Dictionary it;
        it["where"] = gs( inventory_.items[i].where );
        it["name"] = gs( inventory_.items[i].name );
        it["info"] = gs( inventory_.items[i].info );
        it["uid"] = inventory_.items[i].uid;
        it["category"] = gs( inventory_.items[i].category );
        it["meta"] = gs( inventory_.items[i].meta );
        it["key"] = gs( inventory_.items[i].key );
        items[static_cast<int64_t>( i )] = it;
    }
    d["items"] = items;
    return d;
}

} // namespace godot_backend

#endif // GODOT
