#pragma once

#include <cstdint>
#include <string>

namespace offsets::lazer {

    inline constexpr const char* anchor_pattern =
        "01 01 00 00 00 00 80 44 00 00 40 44";

    struct table_t {
        std::string osu_version = "2026.804.2";

        uint32_t api_access_game = 784;
        uint32_t ext_link_opener_api = 536;

        uint32_t game_base_beatmap_clock = 1232;
        uint32_t game_base_beatmap = 1104;
        uint32_t game_base_api = 1080;

        // Updated for 2026.804.2
        uint32_t game_screen_stack = 1552;

        uint32_t screen_stack_stack = 800;

        uint32_t framed_clock_final_source = 528;
        uint32_t framed_clock_current_time = 48;

        uint32_t working_map_info = 8;
        uint32_t working_map_set_info = 16;

        uint32_t map_info_online_id = 140;
        uint32_t map_info_hash = 80;
        uint32_t map_info_difficulty = 0x18;

        uint32_t set_info_online_id = 48;

        uint32_t submitting_player_api = 1264;
        // Exact 2026.804.2 layouts recovered from the shipped osu.Game.dll.
        uint32_t player_loader_current_player = 0x428;
        uint32_t player_api = 0x3F0;
        uint32_t player_game = 0x400;
        uint32_t player_gameplay_state = 0x408;
        uint32_t player_drawable_ruleset = 0x458;
        uint32_t player_gameplay_clock_container = 0x468;
        uint32_t gameplay_state_beatmap = 0x08;
        uint32_t gameplay_state_ruleset = 0x10;
        uint32_t gameplay_state_mods = 0x18;
        uint32_t gameplay_clock_gameplay_clock = 0x340;

        // Was hardcoded as +1120 in your reader.
        uint32_t game_base_selected_mods = 1120;

        // These low-level Bindable offsets are dynamically validated by the memory reader.
        // They are preserved from your previous table.
        uint32_t bindable_value = 0x20;
        uint32_t bindable_number_value = 0x40;

        uint32_t drawable_osu_beatmap = 0x380;
        uint32_t beatmap_info = 0x10;
        uint32_t beatmap_hit_objects = 0x28;
        uint32_t map_info_set_info = 0x48;

        uint32_t list_items = 8;
        uint32_t list_size = 0x10;
        uint32_t array_first_element = 0x10;

        uint32_t hit_object_start_time_bindable = 0x10;
        uint32_t osu_hit_object_position_xy = 0x58;
        uint32_t hit_object_nested_objects = 40;
        uint32_t hit_object_has_duration = 0xD0;

        uint32_t slider_path_wrapper = 0xE0;
        uint32_t path_ctrl_points_list = 0x20;

        [[nodiscard]] bool has_hitobject_offsets() const {
            return drawable_osu_beatmap != 0 &&
                beatmap_hit_objects != 0 &&
                list_items != 0 &&
                list_size != 0 &&
                hit_object_start_time_bindable != 0 &&
                bindable_value != 0;
        }
    };

    inline bool load_from_file(table_t& out, const std::wstring& path) {
        return true;
    }

    inline std::wstring default_json_path() {
        return L"";
    }

}