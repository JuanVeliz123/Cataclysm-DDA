#pragma once
#ifndef CATA_SRC_HELP_H
#define CATA_SRC_HELP_H

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "translation.h"

class JsonObject;

class help
{
    public:
        static void load( const JsonObject &jo, const std::string &src );
        static void reset();
        void display_help() const;
    private:
        void load_object( const JsonObject &jo, const std::string &src );
        void reset_instance();
        static std::string get_note_colors();
        static std::string get_dir_grid();
        // Modifier for each mods order
        int current_order_start = 0;
        std::string current_src;
        std::map<int, std::pair<translation, std::vector<translation>>> help_texts;
};

help &get_help();

std::string get_hint();

#endif // CATA_SRC_HELP_H
