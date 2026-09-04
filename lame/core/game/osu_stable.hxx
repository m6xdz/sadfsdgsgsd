#pragma once

#include <core/game/i_osu_client.hxx>
#include <impl/defs/offsets_stable.hxx>
#include <impl/memory/scanner.hxx>

namespace game {

    class c_osu_stable : public i_osu_client {
    public:
        bool attach( memory::c_process& process ) override {
            memory::c_scanner scanner( process );

            const auto time_scan = scanner.scan( offsets::stable::pattern_time, offsets::stable::pattern_time_offset );
            const auto state_scan = scanner.scan( offsets::stable::pattern_state, offsets::stable::pattern_state_offset );
            const auto beatmap_scan = scanner.scan( offsets::stable::pattern_beatmap, offsets::stable::pattern_beatmap_offset );
            const auto mods_scan = scanner.scan( offsets::stable::pattern_menu_mods, offsets::stable::pattern_menu_mods_offset );

            if ( !time_scan || !state_scan )
                return false;

            m_time_ptr = process.read<uint32_t>( time_scan );
            m_state_ptr = process.read<uint32_t>( state_scan );
            if ( beatmap_scan )
                m_beatmap_base = process.read<uint32_t>( beatmap_scan );
            if ( mods_scan )
                m_mods_ptr = process.read<uint32_t>( mods_scan );

            m_logged_first_play = false;
            m_last_map_id = -1;
            m_last_set_id = 0;
            m_last_folder.clear( );
            m_last_file.clear( );

            return m_time_ptr != 0 && m_state_ptr != 0;
        }

        void update( memory::c_process& process, osu::game_snapshot_t& snap ) override {
            snap.client = osu::client_kind_t::stable;
            snap.game_base = 0;

            if ( !m_time_ptr || !m_state_ptr )
                return;

            snap.cur_time = process.read<int32_t>( m_time_ptr );
            snap.cur_state = static_cast<osu::game_state_t>( process.read<int32_t>( m_state_ptr ) );
            snap.is_replay = snap.cur_state == osu::game_state_t::play &&
                process.read<uint8_t>( m_state_ptr + offsets::stable::game_state_replay_flag ) == 1;

            if ( m_mods_ptr )
                snap.cur_mod_state = process.read<int32_t>( m_mods_ptr );

            snap.speed_mult = 1.f;
            if ( ( snap.cur_mod_state & 64 ) != 0 || ( snap.cur_mod_state & 512 ) != 0 ) {
                snap.speed_mult = 1.5f;
            } else if ( ( snap.cur_mod_state & 256 ) != 0 ) {
                snap.speed_mult = 0.75f;
            }

            if ( m_beatmap_base ) {
                const auto beatmap = process.read<uint32_t>( m_beatmap_base );
                if ( beatmap ) {
                    const int32_t map_id = process.read<int32_t>( beatmap + offsets::stable::beatmap_map_id );
                    if ( map_id != m_last_map_id ) {
                        m_last_map_id = map_id;
                        m_last_set_id = process.read<int32_t>( beatmap + offsets::stable::beatmap_set_id );
                        process.read_dotnet_string( beatmap + offsets::stable::beatmap_folder_name, m_last_folder );
                        process.read_dotnet_string( beatmap + offsets::stable::beatmap_file_name, m_last_file );
                    }
                    snap.map_id = m_last_map_id;
                    snap.set_id = m_last_set_id;
                    snap.map_folder = m_last_folder;
                    snap.map_file = m_last_file;
                }
            }

            if ( snap.cur_state == osu::game_state_t::play && !m_logged_first_play ) {
                m_logged_first_play = true;
            }

            if ( snap.cur_state != osu::game_state_t::play )
                m_logged_first_play = false;
        }

        osu::client_kind_t kind( ) const override { return osu::client_kind_t::stable; }

    private:
        uint64_t m_time_ptr = 0;
        uint64_t m_state_ptr = 0;
        uint64_t m_beatmap_base = 0;
        uint64_t m_mods_ptr = 0;
        bool m_logged_first_play = false;

        int32_t     m_last_map_id = -1;
        int32_t     m_last_set_id = 0;
        std::string m_last_folder;
        std::string m_last_file;
    };

}