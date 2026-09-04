#pragma once

#include <impl/struct/osu_types.hxx>
#include <string>
#include <cstdint>

namespace osu {

    struct game_snapshot_t {
        bool attached = false;
        client_kind_t client = client_kind_t::none;
        int32_t pid = 0;
        int32_t cur_time = 0;
        game_state_t cur_state = game_state_t::main_menu;
        int32_t cur_mod_state = 0;
        int32_t map_id = 0;
        int32_t set_id = 0;
        std::string map_folder;
        std::string map_file;
        std::string beatmap_hash;
        std::string beatmap_version;
        uint64_t game_base = 0;
        uint64_t player_screen = 0;
        uint64_t gameplay_state = 0;
        uint64_t gameplay_beatmap = 0;
        uint64_t gameplay_clock_container = 0;
        uint64_t drawable_ruleset = 0;
        std::string client_version;
        std::string offset_version;
        bool offset_mismatch = false;
        std::string songs_path;
        int32_t left_key = 'S';
        int32_t right_key = 'D';
        float speed_mult = 1.f;
        bool is_replay = false;

        // Lazer diagnostics: these are populated by c_osu_lazer::update().
        // They intentionally expose each link in the pointer chain so a broken
        // offset can be identified from the System tab without a debugger.
        bool diag_clock_ok = false;
        bool diag_beatmap_ok = false;
        bool diag_stack_ok = false;
        bool diag_current_screen_ok = false;
        bool diag_submitting_api_match = false;
        bool diag_player_api_match = false;
        bool diag_ruleset_ok = false;
        int32_t diag_stack_count = 0;
        int32_t diag_raw_time = 0;
        uint64_t diag_beatmap_clock = 0;
        uint64_t diag_final_source = 0;
        uint64_t diag_beatmap_bindable = 0;
        uint64_t diag_working_beatmap = 0;
        uint64_t diag_screen_stack = 0;
        uint64_t diag_stack = 0;
        uint64_t diag_current_screen = 0;
        uint64_t diag_base_api = 0;
        uint64_t diag_submitting_api = 0;
        uint64_t diag_player_api = 0;
        uint64_t diag_drawable_ruleset = 0;

        // Offset probes. These do not alter gameplay state; they only look for
        // plausible replacements for offsets that failed the normal chain.
        int32_t diag_probe_stack_offset = -1;
        int32_t diag_probe_stack_count = 0;
        bool diag_probe_stack_api_match = false;
        bool diag_probe_stack_ruleset = false;
        uint64_t diag_probe_current_screen = 0;

        int32_t diag_probe_bindable_offset = -1;
        int32_t diag_probe_bindable_score = 0;
        int32_t diag_probe_map_id = 0;
        uint64_t diag_probe_working_beatmap = 0;
        std::string diag_probe_difficulty;

        int32_t diag_probe_clock_offset = -1;
        int32_t diag_probe_clock_hits = 0;
        int32_t diag_probe_clock_value = 0;

        // Deeper clock-chain probe: searches the BeatmapClock object itself for
        // a source pointer and then for a double that advances like milliseconds.
        int32_t diag_probe_clock_source_offset = -1;
        int32_t diag_probe_clock_time_offset = -1;
        int32_t diag_probe_clock_chain_hits = 0;
        int32_t diag_probe_clock_chain_value = 0;

        // Age of the last transient probe result that was latched (milliseconds).
        int32_t diag_probe_capture_age_ms = -1;

        // Last non-null diagnostic stages observed while gameplay objects existed.
        bool diag_hold_clock_obj = false;
        bool diag_hold_clock_final = false;
        bool diag_hold_clock_time = false;
        bool diag_hold_beatmap_bind = false;
        bool diag_hold_beatmap_value = false;
        bool diag_hold_stack_obj = false;
        bool diag_hold_stack_inner = false;
        int32_t diag_hold_stack_count = 0;
    };

    struct full_snapshot_t {
        game_snapshot_t game;
        beatmap_data_t beatmap;
    };

}