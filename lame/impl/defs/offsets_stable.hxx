#pragma once

#include <cstdint>

namespace offsets::stable {

    inline constexpr const char* pattern_time = "5E 5F 5D C3 A1 ? ? ? ? 89 ? 04";
    inline constexpr int32_t pattern_time_offset = 5;

    inline constexpr const char* pattern_state = "48 83 F8 04 73 1E";
    inline constexpr int32_t pattern_state_offset = -4;

    inline constexpr const char* pattern_beatmap = "F8 01 74 04 83 65";
    inline constexpr int32_t pattern_beatmap_offset = -12;

    inline constexpr uint32_t beatmap_map_id = 0xC8;
    inline constexpr uint32_t beatmap_set_id = 0xCC;
    inline constexpr uint32_t beatmap_folder_name = 0x74;
    inline constexpr uint32_t beatmap_file_name = 0x94;

    inline constexpr const char* pattern_menu_mods = "81 0D ? ? ? ? 00 08 00 00 C7";
    inline constexpr int32_t pattern_menu_mods_offset = 2;

    inline constexpr uint32_t game_state_replay_flag = 47;

}