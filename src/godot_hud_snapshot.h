#pragma once
#ifndef CATA_SRC_GODOT_HUD_SNAPSHOT_H
#define CATA_SRC_GODOT_HUD_SNAPSHOT_H

#if defined(GODOT)

#include <mutex>
#include <string>
#include <vector>

#include <godot_cpp/variant/dictionary.hpp>

namespace godot_backend
{

/**
 * In-session HUD / inventory / character sheet snapshot for Godot Controls.
 * Game thread fills C++ structs; Godot main thread copies Dictionaries.
 */
class HudSnapshot
{
    public:
        void update_from_game();

        godot::Dictionary copy_hud() const;
        godot::Dictionary copy_character() const;
        godot::Dictionary copy_inventory() const;

        struct kv {
            std::string name;
            std::string detail;
        };
        struct limb {
            std::string name;
            int hp = 0;
            int hp_max = 0;
        };
        struct skill {
            std::string name;
            int level = 0;
            int knowledge = 0;
        };
        struct inv_item {
            std::string where;
            std::string name;
            std::string info;
            /// item::uid, so a Godot panel can act on this row without depending on
            /// its position in a list that may already be stale.
            int64_t uid = 0;
            /// Item category name (AMMO, TOOLS, FOOD ...), for grouped display.
            std::string category;
            /// Right-aligned per-row detail: charges, or how a worn item sits.
            std::string meta;
            /// The item's inventory letter, shown in the row's key box.
            std::string key;
        };
        /// One monster or NPC the player can see, for "02 Contacts".
        struct contact {
            std::string symbol;
            std::string name;
            /// Distance and direction, e.g. "9 W".
            std::string meta;
            /// "bad" / "warn" / "dim" -- the design's three status tones.
            std::string tone;
        };
        /// A status chip in "01 Vitals".
        struct chip {
            std::string label;
            std::string tone;
        };
        /// One line of "03 Log".
        struct log_line {
            std::string stamp;
            std::string text;
            std::string tone;
        };

    private:
        struct hud_data {
            std::string name;
            std::string date;
            std::string time;
            std::string location;
            std::string weather;
            std::string hunger;
            std::string thirst;
            std::string sleepiness;
            std::string pain;
            std::string move_mode;
            std::string carry;
            std::string temperature;
            std::string weapon;
            std::string style;
            std::string threat_summary;
            std::vector<chip> effects;
            std::vector<contact> contacts;
            /// Bionic power as a percentage, or -1 when the character has none.
            int power_pct = -1;
            /// The design's four-cell metrics grid.
            int focus = 0;
            int speed = 0;
            int sound = 0;
            int pain_level = 0;
            /// Signed, as the design's centre-zero morale bar needs.
            int morale_level = 0;
            /// Single-character item symbol for the wielded weapon.
            std::string weapon_glyph;
            /// Visible-creature counts per direction, in the order the design's
            /// 3x3 grid reads: NW N NE W E SW S SE.
            std::vector<int> compass;
            int hp = 0;
            int hp_max = 0;
            int stamina = 0;
            int stamina_max = 0;
            int str = 0;
            int dex = 0;
            int intel = 0;
            int per = 0;
            std::vector<log_line> messages;
            std::vector<limb> limbs;
        };
        struct character_data {
            std::string name;
            int str = 0;
            int dex = 0;
            int intel = 0;
            int per = 0;
            std::vector<limb> limbs;
            std::vector<skill> skills;
            std::vector<kv> traits;
            std::vector<kv> bionics;
            std::vector<kv> status;
        };
        struct inventory_data {
            /// Pre-formatted "used / capacity unit" readouts.
            std::string carry;
            std::string volume;
            /// The same as percentages, for the design's segmented gauges.
            int carry_pct = 0;
            int volume_pct = 0;
            std::vector<inv_item> items;
        };

        mutable std::mutex mutex_;
        hud_data hud_;
        character_data character_;
        inventory_data inventory_;
};

HudSnapshot &get_hud_snapshot();
void update_hud_snapshot();

} // namespace godot_backend

#endif // GODOT
#endif // CATA_SRC_GODOT_HUD_SNAPSHOT_H
