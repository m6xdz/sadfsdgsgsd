#pragma once

#include <core/replay/osr_decoder.hxx>
#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <impl/util/playfield.hxx>
#include <Windows.h>
#include <cmath>
#include <string>
#include <algorithm>
#include <vector>
#include <cstdint>

namespace replay {

    static std::string key_mask_to_string( int mask ) {
        static const char* names[] = {
            "M1", "M2", "K1", "K2", "K3", "K4", "Smoke", "ResetMods",
            "K5", "K6", "K7", "K8", "K9", "Custom1", "Custom2", "KustomCustomSing"
        };
        std::string result;
        for ( int i = 0; i < 16; ++i ) {
            if ( mask & ( 1 << i ) ) {
                if ( !result.empty( ) ) result += ",";
                result += names[ i ];
            }
        }
        if ( result.empty( ) ) result = "None";
        return result;
    }

    class c_replay_bot {
    public:
        bool enabled = false;
        bool parse_buttons = true;
        std::wstring replay_path;

        bool load_replay( ) {
            c_osr_decoder decoder;
            m_replay = {};
            m_last_load_error.clear( );
            reset_sync( );

            if ( replay_path.empty( ) ) {
                m_last_load_error = "no replay path set";
                return false;
            }

            const bool ok = decoder.load( replay_path, m_replay );
            m_last_load_error = m_replay.last_error.empty( ) ? decoder.last_error( ) : m_replay.last_error;

            if ( ok && m_replay.valid && !m_replay.frames.empty( ) ) {
                build_stats( );
                m_frame_data = m_replay.frames;
            }

            return ok;
        }

        [[nodiscard]] size_t frame_count( ) const { return m_frame_data.size( ); }
        [[nodiscard]] bool replay_valid( ) const { return m_replay.valid; }
        [[nodiscard]] const std::string& last_load_error( ) const { return m_last_load_error; }
        [[nodiscard]] const replay_data_t& replay_data( ) const { return m_replay; }
        [[nodiscard]] const std::string& player_name( ) const { return m_replay.player_name; }

        void reset_sync( ) {
            m_frame_index = 0;
            m_last_game_time = -1;
            m_synced = false;
            m_driving_cursor = false;
            release_keys( );
        }

        void on_leave_play( const osu::game_snapshot_t& game ) {
            release_keys( );
            m_frame_index = 0;
            m_last_game_time = -1;
            m_synced = false;
            m_driving_cursor = false;
            m_left_vk = static_cast<WORD>( game.left_key ? game.left_key : 'Z' );
            m_right_vk = static_cast<WORD>( game.right_key ? game.right_key : 'X' );
        }

        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t&, bool user_control = false ) {
            if ( !enabled || !m_replay.valid || game.cur_state != osu::game_state_t::play )
                return;

            if ( user_control ) {
                if ( m_driving_cursor ) {
                    release_keys( );
                    m_driving_cursor = false;
                }
                return;
            }

            m_driving_cursor = true;

            m_left_vk = static_cast<WORD>( game.left_key ? game.left_key : 'Z' );
            m_right_vk = static_cast<WORD>( game.right_key ? game.right_key : 'X' );

            const HWND hwnd = input::target_window( );
            if ( !hwnd )
                return;

            RECT window{};
            if ( !playfield::get_playfield_rect( hwnd, window ) )
                return;

            const int screen_w = window.right - window.left;
            const int screen_h = window.bottom - window.top;
            if ( screen_w <= 1 || screen_h <= 1 )
                return;

            const int game_time = game.cur_time;
            const auto& frames = m_frame_data;
            if ( frames.size( ) < 2 )
                return;

            if ( !m_synced || game_time < m_last_game_time - 200 ) {
                m_frame_index = 0;
                m_synced = true;
            }
            m_last_game_time = game_time;

            while ( m_frame_index + 1 < static_cast<int>( frames.size( ) ) &&
                frames[ static_cast<size_t>( m_frame_index + 1 ) ].absolute_time <= game_time ) {
                ++m_frame_index;
            }

            if ( m_frame_index >= static_cast<int>( frames.size( ) ) - 1 )
                return;

            const int idx = std::clamp( m_frame_index, 0, static_cast<int>( frames.size( ) ) - 1 );
            const auto& cur = frames[ static_cast<size_t>( idx ) ];

            float x = cur.x;
            float y = cur.y;

            if ( m_frame_index + 1 < static_cast<int>( frames.size( ) ) ) {
                const auto& next = frames[ static_cast<size_t>( m_frame_index + 1 ) ];
                if ( next.absolute_time > cur.absolute_time ) {
                    const float t = std::clamp(
                        static_cast<float>( game_time - cur.absolute_time ) /
                        static_cast<float>( next.absolute_time - cur.absolute_time ),
                        0.f, 1.f );
                    x = cur.x + ( next.x - cur.x ) * t;
                    y = cur.y + ( next.y - cur.y ) * t;
                }
            }

            const bool game_has_hr = ( game.cur_mod_state & 16 ) != 0;
            const bool replay_has_hr = ( m_replay.mods & 16 ) != 0;
            if ( game_has_hr != replay_has_hr )
                y = 384.f - y;

            const auto target_p = playfield::playfield_to_screen( x, y, window, 17 );
            input::move_absolute_virtual_desktop( target_p.x, target_p.y );

            if ( parse_buttons ) {
                const bool want_left = ( cur.keys & 1 ) != 0 || ( cur.keys & 4 ) != 0;
                const bool want_right = ( cur.keys & 2 ) != 0 || ( cur.keys & 8 ) != 0;
                set_key_state( m_left_vk, want_left, m_left_down );
                set_key_state( m_right_vk, want_right, m_right_down );
            }
        }

    private:
        replay_data_t m_replay;
        std::vector<frame_t> m_frame_data;
        std::string m_last_load_error;
        int m_frame_index = 0;
        int32_t m_last_game_time = -1;
        bool m_synced = false;
        bool m_left_down = false;
        bool m_right_down = false;
        bool m_driving_cursor = false;
        WORD m_left_vk = 'Z';
        WORD m_right_vk = 'X';

        void build_stats( ) {
        }

        void release_keys( ) {
            if ( m_left_down ) {
                input::release_vk( m_left_vk );
                m_left_down = false;
            }
            if ( m_right_down ) {
                input::release_vk( m_right_vk );
                m_right_down = false;
            }
        }

        static void set_key_state( WORD vk, bool down, bool& state ) {
            if ( !vk || state == down )
                return;
            input::key_event( vk, down );
            state = down;
        }
    };

}