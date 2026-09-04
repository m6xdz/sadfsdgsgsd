#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <core/aim_assist/a_core.hxx>
#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <impl/util/playfield.hxx>
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <climits>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>
#include <memory>

namespace aim_assist {

    struct aim_snapshot_t {
        std::vector<assist::waypoint_t> targets;
        RECT window{};
        float speed_mult = 1.f;
        bool hr = false;
        bool ez = false;
        float cs = 5.f;
    };

    class c_aimbot {
    public:
        bool  enabled         = false;
        bool  ignore_sliders  = false;
        bool  tablet_mode     = false;
        bool  legit_mode      = true;
        float strength_x       = 5.0f;
        float strength_y       = 4.5f;
        float decay_far        = 0.02f;
        float aim_lerp         = 0.24f;
        float aim_window       = 100.f;
        float freeze_lerp      = 0.17f;
        float legit_clamp      = 1.3f;


        void start() {
            clear_motion_state();
        }

        void stop() {
            clear_motion_state();
        }

        void on_leave_play( ) {
            m_in_play.store( false );
            m_user_blocked.store( false );
            clear_motion_state( );
            {
                std::lock_guard<std::mutex> slock( m_snap_mutex );
                m_shared_snap = std::make_shared<aim_snapshot_t>( );
            }
        }

        void set_user_input_blocked( bool blocked ) {
            const bool was = m_user_blocked.exchange( blocked );
            if ( blocked && !was )
                clear_motion_state( );
        }

        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            if ( m_user_blocked.load( ) ) {
                m_in_play.store( false );
                return;
            }

            const bool in_play = enabled
                                 && game.cur_state == osu::game_state_t::play
                                 && map.loaded
                                 && !map.objects.empty( );
            m_in_play.store( in_play );

            if ( !in_play ) {
                {
                    std::lock_guard<std::mutex> slock( m_snap_mutex );
                    m_shared_snap = std::make_shared<aim_snapshot_t>( );
                }
                return;
            }

            const HWND hwnd = input::target_window( );
            RECT window{};
            if ( !hwnd || !playfield::get_playfield_rect( hwnd, window ) ) {
                {
                    std::lock_guard<std::mutex> slock( m_snap_mutex );
                    m_shared_snap = std::make_shared<aim_snapshot_t>( );
                }
                return;
            }

            const int win_w = window.right - window.left;
            const int win_h = window.bottom - window.top;

            if ( window.left != m_window.left || window.top != m_window.top || 
                 window.right != m_window.right || window.bottom != m_window.bottom ) {
                m_window = window;
                input::invalidate_virtual_desktop( );
            }

            const bool hr_active = ( game.cur_mod_state & 16 ) != 0;
            const bool ez_active = ( game.cur_mod_state & 2 ) != 0;

            float effective_cs = map.cs;
            if ( hr_active && !map.hr )
                effective_cs = std::min( effective_cs * 1.3f, 10.f );
            else if ( ez_active && !map.ez )
                effective_cs = effective_cs * 0.5f;

            auto snap = std::make_shared<aim_snapshot_t>( );
            snap->window = window;
            snap->speed_mult = game.speed_mult;
            snap->hr = hr_active;
            snap->ez = ez_active;
            snap->cs = effective_cs;
            snap->targets.reserve( 16 );

            for ( const auto& obj : map.objects ) {
                if ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::spinner ) )
                    continue;
                
                if ( game.cur_time >= obj.start_time )
                    continue;

                assist::waypoint_t t{};
                t.x = obj.x;
                t.y = obj.y;

                if ( hr_active && !map.hr )
                    t.y = 384.f - obj.y;

                t.start_time = obj.start_time;
                t.end_time = obj.end_time;
                t.game_time = game.cur_time;
                t.hit_radius = hit_radius_screen( effective_cs, win_w, win_h );
                t.alive = true;
                t.is_slider = ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) != 0;
                
                snap->targets.push_back( t );
            }

            {
                std::lock_guard<std::mutex> slock( m_snap_mutex );
                m_shared_snap = snap;
            }
        }

        std::optional<POINT> apply_hook_move( POINT raw, const MSLLHOOKSTRUCT& ) {
            if ( !enabled || m_user_blocked.load( ) || !m_in_play.load( ) )
                return std::nullopt;

            float cursor_x = static_cast<float>( raw.x );
            float cursor_y = static_cast<float>( raw.y );

            if ( !tablet_mode ) {
                cursor_x -= m_prev_injected_x;
                cursor_y -= m_prev_injected_y;
            }

            POINT assisted{};
            if ( try_apply_absolute( cursor_x, cursor_y, &assisted ) ) {
                return assisted;
            }

            return std::nullopt;
        }

    private:
        std::atomic<bool>                  m_in_play{ false };
        std::atomic<bool>                  m_user_blocked{ false };
        RECT                               m_window{};
        std::shared_ptr<aim_snapshot_t>     m_shared_snap = std::make_shared<aim_snapshot_t>( );
        std::mutex                         m_snap_mutex;
        assist::trace                 m_state{};
        std::mutex                         m_apply_mutex;
        float m_prev_injected_x = 0.f;
        float m_prev_injected_y = 0.f;

        void clear_motion_state( ) {
            std::lock_guard<std::mutex> lock( m_apply_mutex );
            m_window = {};
            m_state = {};
            m_prev_injected_x = 0.f;
            m_prev_injected_y = 0.f;
        }

        assist::config build_config( float speed_mult ) const {
            assist::config cfg{};
            cfg.enabled        = enabled;
            cfg.ignore_sliders = ignore_sliders;
            cfg.legit_mode     = legit_mode;
            cfg.strength_x     = strength_x;
            cfg.strength_y     = strength_y;
            cfg.decay_far      = decay_far;
            cfg.aim_lerp       = aim_lerp;
            cfg.freeze_lerp   = freeze_lerp;
            cfg.aim_window     = aim_window;
            cfg.legit_clamp    = legit_clamp;
            cfg.speed_mult     = speed_mult;

            return cfg;
        }

        bool try_apply_absolute( float cursor_x, float cursor_y, POINT* out_pos ) {
            std::lock_guard<std::mutex> lock( m_apply_mutex );

            std::shared_ptr<aim_snapshot_t> snap;
            {
                std::lock_guard<std::mutex> slock( m_snap_mutex );
                snap = m_shared_snap;
            }

            const bool has_targets = snap && !snap->targets.empty( );
            const bool has_drift = ( std::abs( m_state.delta_x ) > 0.05f || std::abs( m_state.delta_y ) > 0.05f );

            if ( !has_targets && !has_drift )
                return false;

            if ( has_targets ) {
                auto cfg = build_config( snap->speed_mult );
                float out_x = 0.f;
                float out_y = 0.f;

                if ( !assist::adjust( cursor_x, cursor_y, snap->targets.data(), snap->targets.size(), snap->window, cfg, m_state, &out_x, &out_y ) )
                    return false;

                out_pos->x = static_cast<int>( std::lround( out_x ) );
                out_pos->y = static_cast<int>( std::lround( out_y ) );

                input::move_absolute_virtual_desktop( out_pos->x, out_pos->y );

                m_prev_injected_x = out_x - cursor_x;
                m_prev_injected_y = out_y - cursor_y;
                return true;
            }

            constexpr float fade_rate = 0.15f;
            m_state.delta_x *= ( 1.f - fade_rate );
            m_state.delta_y *= ( 1.f - fade_rate );
            if ( std::abs( m_state.delta_x ) < 0.1f ) m_state.delta_x = 0.f;
            if ( std::abs( m_state.delta_y ) < 0.1f ) m_state.delta_y = 0.f;

            float out_x = cursor_x + m_state.delta_x;
            float out_y = cursor_y + m_state.delta_y;

            out_pos->x = static_cast<int>( std::lround( out_x ) );
            out_pos->y = static_cast<int>( std::lround( out_y ) );

            input::move_absolute_virtual_desktop( out_pos->x, out_pos->y );

            m_prev_injected_x = m_state.delta_x;
            m_prev_injected_y = m_state.delta_y;
            return true;
        }

        static float hit_radius_screen( float cs, int win_w, int win_h ) {
            const float playfield_height = static_cast<float>( win_h ) * 0.8f;
            const float osu_scale = ( playfield_height * ( 4.f / 3.f ) ) / 512.f;
            const float osu_radius = 54.4f - 4.48f * cs;
            return std::max( 8.f, osu_radius * osu_scale );
        }
    };

}