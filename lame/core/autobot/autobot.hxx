#pragma once

#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <impl/util/playfield.hxx>
#include <Windows.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>
#include <sstream>

namespace autobot {

    inline uint64_t get_time_ms( ) {
        static LARGE_INTEGER freq;
        static bool init = false;
        if ( !init ) {
            QueryPerformanceFrequency( &freq );
            init = true;
        }
        LARGE_INTEGER time;
        QueryPerformanceCounter( &time );
        return static_cast<uint64_t>( static_cast<double>( time.QuadPart ) / static_cast<double>( freq.QuadPart ) * 1000.0 );
    }

    struct point_t {
        float x = 0.f;
        float y = 0.f;
    };

    class c_autobot {
    public:
        bool enabled = false;
        float aim_spread = 0.15f;
        float curve_strength = 0.33f;
        float drift_amount = 1.5f;
        float momentum = 0.85f;
        float slider_laziness = 0.15f;
        float spinner_rpm = 400.f;

        void on_leave_play( const osu::game_snapshot_t& game ) {
            release_keys( );
            m_click_queue.clear( );
            m_slider_cache.clear( );
            m_scheduled_through_idx = -1;
            m_last_active_idx = -1;
            m_synced = false;
            m_in_play = false;
            m_driving_cursor = false;
            m_use_k2_next = false;
            m_last_click_time = -99999;
            m_spinner_active = false;
            m_spinner_start_time = -1;
            m_cursor_x = -1.f;
            m_cursor_y = -1.f;
            m_seed_x = static_cast<float>( rand() % 10000 );
            m_seed_y = static_cast<float>( rand() % 10000 );
            m_slider_active = false;
            m_slider_entry_time = -1;
            m_slider_entry_x = 0.f;
            m_slider_entry_y = 0.f;
            m_bezier_valid = false;
            m_last_note_idx = -1;
            m_bezier_start_ticks = 0;
            m_bezier_ticks_per_ms = 0.0;
            m_estimated_rate = 1.0;
            m_rate_ref_real_time = 0.0;
            m_rate_ref_game_time = 0;
        }

        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map, bool user_control = false ) {
            if ( !enabled || user_control ) {
                if ( m_in_play )
                    on_leave_play( game );
                return;
            }

            const HWND hwnd = input::target_window( );
            RECT window{};
            if ( !hwnd || !playfield::get_playfield_rect( hwnd, window ) ) {
                if ( m_in_play )
                    on_leave_play( game );
                return;
            }

            m_driving_cursor = true;
            m_in_play = true;
            const int gt = game.cur_time;
            const int win_w = window.right - window.left;
            const int win_h = window.bottom - window.top;

            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );
            m_k1_actual = k1;
            m_k2_actual = k2;

            if ( !m_synced || gt < m_last_game_time - 200 ) {
                release_keys( );
                m_click_queue.clear( );
                m_slider_cache.clear( );
                m_scheduled_through_idx = -1;
                m_last_active_idx = -1;
                m_use_k2_next = false;
                m_last_click_time = -99999;
                m_spinner_active = false;
                m_spinner_start_time = -1;
                m_cursor_x = -1.f;
                m_cursor_y = -1.f;
                m_seed_x = static_cast<float>( rand() % 10000 );
                m_seed_y = static_cast<float>( rand() % 10000 );
                m_slider_entry_time = -1;
                m_slider_entry_x = 0.f;
                m_slider_entry_y = 0.f;
                m_slider_active = false;
                m_bezier_valid = false;
                m_last_note_idx = -1;
                m_bezier_start_ticks = 0;
                m_bezier_ticks_per_ms = 0.0;
                m_synced = true;
                m_estimated_rate = 1.0;
                m_rate_ref_real_time = 0.0;
                m_rate_ref_game_time = 0;
            }
            m_last_game_time = gt;

            double real_now = 0.0;
            LARGE_INTEGER qpc_count, qpc_freq;
            if ( QueryPerformanceCounter( &qpc_count ) && QueryPerformanceFrequency( &qpc_freq ) ) {
                real_now = ( static_cast<double>( qpc_count.QuadPart ) / qpc_freq.QuadPart ) * 1000.0;
            }

            if ( m_rate_ref_real_time == 0.0 || gt < m_rate_ref_game_time || gt > m_rate_ref_game_time + 5000 ) {
                m_rate_ref_real_time = real_now;
                m_rate_ref_game_time = gt;
                m_estimated_rate = 1.0;
            } else {
                double real_elapsed = real_now - m_rate_ref_real_time;
                if ( real_elapsed >= 250.0 ) {
                    int game_elapsed = gt - m_rate_ref_game_time;
                    if ( game_elapsed > 0 ) {
                        double raw_rate = static_cast<double>( game_elapsed ) / real_elapsed;
                        if ( raw_rate > 1.25 ) {
                            m_estimated_rate = 1.5;
                        } else if ( raw_rate < 0.85 ) {
                            m_estimated_rate = 0.75;
                        } else {
                            m_estimated_rate = 1.0;
                        }
                    }
                    m_rate_ref_real_time = real_now;
                    m_rate_ref_game_time = gt;
                }
            }

            schedule_clicks( game, map );

            const int note_idx = find_current_note( gt, map );
            point_t target_osu{};
            bool is_slider_note = false;
            bool is_spinner_note = false;

            if ( note_idx >= 0 ) {
                const auto& obj = map.objects[ static_cast<size_t>( note_idx ) ];

                if ( is_spinner_object( obj ) ) {
                    is_spinner_note = true;
                    m_slider_active = false;
                    target_osu = get_spinner_position( obj, gt );
                }
                else if ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) {
                    is_slider_note = true;
                    is_spinner_note = false;

                    if ( !m_slider_active ) {
                        m_slider_entry_time = gt;
                        m_slider_entry_x = m_cursor_x >= 0.f ? m_cursor_x : obj.x;
                        m_slider_entry_y = m_cursor_y >= 0.f ? m_cursor_y : obj.y;
                        m_slider_active = true;
                    }

                    point_t ball = evaluate_slider( obj, gt );
                    int elapsed_slider = gt - m_slider_entry_time;
                    if ( elapsed_slider < 100 && elapsed_slider >= 0 ) {
                        float st = static_cast<float>( elapsed_slider ) * 0.01f;
                        st = std::clamp( st, 0.f, 1.f );
                        float ease = st * st * ( 3.f - 2.f * st );
                        target_osu.x = m_slider_entry_x + ( ball.x - m_slider_entry_x ) * ease;
                        target_osu.y = m_slider_entry_y + ( ball.y - m_slider_entry_y ) * ease;
                    } else {
                        target_osu = ball;
                    }
                }
                else {
                    m_slider_active = false;
                    target_osu = { obj.x, obj.y };
                }
            }
            else {
                m_slider_active = false;
                target_osu = get_target_position_osu( game, map );
            }

            if ( note_idx != m_last_note_idx ) {
                m_bezier_valid = false;
                m_last_note_idx = note_idx;
            }
            if ( note_idx < 0 && !m_bezier_valid ) {
                compute_bezier_path( gt, map );
            }

            float v61 = momentum * 0.85f;

            if ( is_slider_note || is_spinner_note ) {
                v61 = std::max( v61, 0.3f );
            }

            if ( !is_spinner_note && note_idx >= 0 ) {
                const auto& obj = map.objects[ static_cast<size_t>( note_idx ) ];
                int dt_since_start = gt - obj.start_time;
                if ( static_cast<unsigned>( dt_since_start ) <= 79u ) {
                    float ramp_scale = static_cast<float>( dt_since_start ) * 0.0125f;
                    ramp_scale = std::clamp( ramp_scale, 0.f, momentum );
                    v61 = v61 + ( momentum - ramp_scale ) * ( momentum - v61 );
                }
            }

            if ( note_idx >= 0 && note_idx > 0 ) {
                const auto& prev_obj = map.objects[ static_cast<size_t>( note_idx - 1 ) ];
                int prev_end = get_movement_end_time( prev_obj, map, note_idx - 1 );
                if ( gt - prev_end < 150 ) {
                    v61 = momentum;
                }
            }

            if ( note_idx < 0 ) {
                v61 = momentum;
            }

            v61 = std::clamp( v61, 0.f, std::min( momentum, 0.98f ) );

            if ( m_cursor_x < 0.f ) {
                m_cursor_x = target_osu.x;
                m_cursor_y = target_osu.y;
            }

            m_cursor_x = m_cursor_x + ( target_osu.x - m_cursor_x ) * v61;
            m_cursor_y = m_cursor_y + ( target_osu.y - m_cursor_y ) * v61;

            m_cursor_x = std::clamp( m_cursor_x, 0.f, 512.f );
            m_cursor_y = std::clamp( m_cursor_y, 0.f, 384.f );

            float wx = 0.f, wy = 0.f;
            playfield::project_osu_to_window( m_cursor_x, m_cursor_y, win_w, win_h, wx, wy, 0 );

            const int sx = static_cast<int>( window.left + wx );
            const int sy = static_cast<int>( window.top + wy );
            POINT cur{};
            if ( input::get_cursor_pos( &cur ) ) {
                const int dx = sx - cur.x;
                const int dy = sy - cur.y;
                input::move_relative( dx, dy );
            } else {
                input::move_absolute_virtual_desktop( sx, sy );
            }

            flush_queue( gt );
        }

    private:
        struct scheduled_click_t {
            int press_time = 0;
            int release_time = 0;
            WORD key = 0;
            bool pressed = false;
            bool released = false;
        };

        struct cached_slider_t {
            int start_time = -1;
            std::vector<point_t> path;
            std::vector<float> dists;
            float total_dist = 0.f;
        };

        std::vector<scheduled_click_t> m_click_queue;
        std::vector<cached_slider_t> m_slider_cache;
        int m_scheduled_through_idx = -1;
        int m_last_active_idx = -1;
        bool m_in_play = false;
        bool m_synced = false;
        int m_last_game_time = 0;
        bool m_k1_down = false;
        bool m_k2_down = false;
        bool m_driving_cursor = false;
        bool m_use_k2_next = false;
        int  m_last_click_time = -99999;
        WORD m_k1_actual = 0;
        WORD m_k2_actual = 0;
        bool m_spinner_active = false;
        int m_spinner_start_time = -1;
        float m_spinner_angle = 0.f;
        uint64_t m_spinner_last_wall_ms = 0;
        float m_cursor_x = -1.f;
        float m_cursor_y = -1.f;
        float m_seed_x = 0.f;
        float m_seed_y = 0.f;

        bool m_slider_active = false;
        int m_slider_entry_time = -1;
        float m_slider_entry_x = 0.f;
        float m_slider_entry_y = 0.f;

        bool m_bezier_valid = false;
        int m_last_note_idx = -1;
        float m_bezier_cp0x = 0.f, m_bezier_cp0y = 0.f;
        float m_bezier_cp1x = 0.f, m_bezier_cp1y = 0.f;
        float m_bezier_cp2x = 0.f, m_bezier_cp2y = 0.f;
        float m_bezier_cp3x = 0.f, m_bezier_cp3y = 0.f;
        int m_bezier_start_time = 0;
        int m_bezier_end_time = 0;
        int64_t m_bezier_start_ticks = 0;
        double m_bezier_ticks_per_ms = 0.0;
        double m_estimated_rate = 1.0;
        double m_rate_ref_real_time = 0.0;
        int m_rate_ref_game_time = 0;

        static void resolve_keys( const osu::game_snapshot_t& game, WORD& k1, WORD& k2 ) {
            k1 = static_cast<WORD>( game.left_key );
            k2 = static_cast<WORD>( game.right_key );
            if ( !k1 ) k1 = 'Z';
            if ( !k2 ) k2 = 'X';
        }

        void release_keys( ) {
            if ( m_k1_down ) { input::release_vk( m_k1_actual ); m_k1_down = false; }
            if ( m_k2_down ) { input::release_vk( m_k2_actual ); m_k2_down = false; }
        }

        void press_key( WORD vk, bool& down ) {
            if ( !vk || down ) return;
            input::press_vk( vk );
            down = true;
        }

        void release_key( WORD vk, bool& down ) {
            if ( !vk || !down ) return;
            input::release_vk( vk );
            down = false;
        }

        static constexpr float k_spinner_omega_rad_per_ms = 477.f * 6.283185307f / 60000.f;

        static bool is_spinner_object( const osu::hit_object_t& obj ) {
            if ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::spinner ) )
                return true;

            if ( !( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) )
                return false;
            if ( !obj.slider_curve_str.empty( ) || obj.slider_length > 0.f )
                return false;

            const int dur = obj.end_time - obj.start_time;
            if ( dur < 400 )
                return false;

            const float dx = obj.x - 256.f;
            const float dy = obj.y - 192.f;
            return dx * dx + dy * dy < 48.f * 48.f;
        }

        static point_t spinner_osu_at_angle( const osu::hit_object_t& obj, float angle ) {
            constexpr float k_center_x = 256.f;
            constexpr float k_center_y = 192.f;
            constexpr float k_radius = 90.f;

            const float cx = ( std::abs( obj.x ) > 1.f || std::abs( obj.y ) > 1.f ) ? obj.x : k_center_x;
            const float cy = ( std::abs( obj.x ) > 1.f || std::abs( obj.y ) > 1.f ) ? obj.y : k_center_y;
            return { cx + std::cos( angle ) * k_radius, cy + std::sin( angle ) * k_radius };
        }

        void drive_spinner_motion( const osu::hit_object_t& obj, const RECT& window, int win_w, int win_h ) {
            const uint64_t now = get_time_ms( );

            if ( !m_spinner_active || m_spinner_start_time != obj.start_time ) {
                m_spinner_active = true;
                m_spinner_start_time = obj.start_time;
                m_spinner_angle = 0.f;
                m_spinner_last_wall_ms = now;

                const point_t raw = spinner_osu_at_angle( obj, 0.f );
                float wx = 0.f, wy = 0.f;
                playfield::project_osu_to_window( raw.x, raw.y, win_w, win_h, wx, wy, 0 );
                const int sx = static_cast<int>( window.left + wx );
                const int sy = static_cast<int>( window.top + wy );
                POINT cur{};
                if ( input::get_cursor_pos( &cur ) ) {
                    const int dx = sx - cur.x;
                    const int dy = sy - cur.y;
                    input::move_relative( dx, dy );
                } else {
                    input::move_absolute_virtual_desktop( sx, sy );
                }
                return;
            }

            float dt = static_cast<float>( now - m_spinner_last_wall_ms );
            m_spinner_last_wall_ms = now;
            if ( dt <= 0.f )
                return;
            if ( dt > 100.f )
                dt = 16.f;

            const float prev_angle = m_spinner_angle;
            m_spinner_angle += dt * k_spinner_omega_rad_per_ms;

            const point_t p0 = spinner_osu_at_angle( obj, prev_angle );
            const point_t p1 = spinner_osu_at_angle( obj, m_spinner_angle );
            float w0x = 0.f, w0y = 0.f, w1x = 0.f, w1y = 0.f;
            playfield::project_osu_to_window( p0.x, p0.y, win_w, win_h, w0x, w0y, 0 );
            playfield::project_osu_to_window( p1.x, p1.y, win_w, win_h, w1x, w1y, 0 );

            const int dx = static_cast<int>( window.left + w1x ) - static_cast<int>( window.left + w0x );
            const int dy = static_cast<int>( window.top + w1y ) - static_cast<int>( window.top + w0y );
            if ( dx == 0 && dy == 0 )
                return;

            if ( !input::move_screen_delta( dx, dy ) )
                input::move_relative( dx, dy );
        }

        static int find_spinner_at_time( int gt, const osu::beatmap_data_t& map ) {
            int best_idx = -1;
            int best_start = -1;
            for ( size_t i = 0; i < map.objects.size( ); ++i ) {
                const auto& obj = map.objects[ i ];
                if ( !is_spinner_object( obj ) )
                    continue;
                if ( gt < obj.start_time - 8 || gt > obj.end_time + 8 )
                    continue;
                if ( obj.start_time >= best_start ) {
                    best_start = obj.start_time;
                    best_idx = static_cast<int>( i );
                }
            }
            return best_idx;
        }

        static int circle_hold_ms( int press_time, int next_start_time ) {
            int gap = next_start_time - press_time;
            if ( gap <= 0 || gap >= 99999 )
                return 22;

            if ( gap <= 80 )
                return std::max( 5, gap / 2 - 2 );

            int hold = 22;
            if ( hold > gap / 2 )
                hold = gap / 2;
            return std::max( 5, hold );
        }

        void schedule_clicks( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );

            for ( int i = m_scheduled_through_idx + 1; i < static_cast<int>( map.objects.size( ) ); ++i ) {
                const auto& obj = map.objects[ static_cast<size_t>( i ) ];

                if ( is_spinner_object( obj ) ) {
                    m_use_k2_next = !m_use_k2_next;
                    const WORD chosen = m_use_k2_next ? k2 : k1;
                    if ( chosen ) {
                        int release_time = obj.end_time;
                        if ( release_time <= obj.start_time )
                            release_time = obj.start_time + 1000;
                        m_click_queue.push_back( { obj.start_time, release_time, chosen, false, false } );
                        m_last_click_time = obj.start_time;
                    }
                    m_scheduled_through_idx = i;
                    continue;
                }

                const int press_time = obj.start_time;
                int release_time = obj.end_time;

                if ( !( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) ) {
                    int next_start = press_time + 500;
                    if ( i + 1 < static_cast<int>( map.objects.size( ) ) )
                        next_start = map.objects[ static_cast<size_t>( i + 1 ) ].start_time;

                    const int hold = circle_hold_ms( press_time, next_start );
                    release_time = press_time + hold;
                }

                m_use_k2_next = !m_use_k2_next;
                const WORD chosen = m_use_k2_next ? k2 : k1;
                if ( !chosen )
                    continue;

                m_click_queue.push_back( { press_time, release_time, chosen, false, false } );
                m_last_click_time = press_time;
                m_scheduled_through_idx = i;
            }
        }

        void flush_queue( int gt ) {
            const WORD k1 = m_k1_actual;
            const WORD k2 = m_k2_actual;

            for ( auto& c : m_click_queue ) {
                if ( c.pressed && !c.released && gt >= c.release_time ) {
                    bool& ref = ( c.key == k1 ) ? m_k1_down : m_k2_down;
                    release_key( c.key, ref );
                    c.released = true;
                }
            }

            for ( auto& c : m_click_queue ) {
                if ( c.pressed || c.released )
                    continue;
                if ( gt < c.press_time )
                    continue;

                bool& ref = ( c.key == k1 ) ? m_k1_down : m_k2_down;
                if ( ref )
                    release_key( c.key, ref );

                press_key( c.key, ref );
                c.pressed = true;
            }

            m_click_queue.erase(
                std::remove_if( m_click_queue.begin( ), m_click_queue.end( ),
                    []( const scheduled_click_t& c ) { return c.released; } ),
                m_click_queue.end( ) );
        }

        int get_movement_end_time( const osu::hit_object_t& obj, const osu::beatmap_data_t& map, int idx ) {
            if ( is_spinner_object( obj ) )
                return obj.end_time;
            if ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) )
                return obj.end_time;

            int next_start = obj.start_time + 500;
            if ( idx + 1 < static_cast<int>( map.objects.size( ) ) )
                next_start = map.objects[ static_cast<size_t>( idx + 1 ) ].start_time;

            return obj.start_time + circle_hold_ms( obj.start_time, next_start ) + 8;
        }

        static float get_noise_2d( float time, float seed ) {
            float term1 = std::sin( seed * 3.0f + time * 0.0059f ) * 0.2f;
            float term2 = std::sin( time * 0.0113f + seed * 2.1f ) * 0.1f;
            float term3 = std::sin( time * 0.0031f + seed * 1.3f ) * 0.3f;
            float term4 = std::sin( time * 0.0017f + seed ) * 0.4f;
            return term1 + term2 + term3 + term4;
        }

        point_t get_spinner_position( const osu::hit_object_t& obj, int gt ) {
            const float elapsed = static_cast<float>( gt - obj.start_time ) * 0.001f;
            
            constexpr float k_center_x = 256.f;
            constexpr float k_center_y = 192.f;
            const float cx = ( std::abs( obj.x ) > 1.f || std::abs( obj.y ) > 1.f ) ? obj.x : k_center_x;
            const float cy = ( std::abs( obj.y ) > 1.f || std::abs( obj.y ) > 1.f ) ? obj.y : k_center_y;

            float omega_multiplier = 0.0523599f;
            
            float cos_x = std::cos( elapsed * 0.2f + m_seed_x * 0.1f );
            float cos_x_start = std::cos( m_seed_x * 0.1f );
            float x_term = ( cos_x - cos_x_start ) * -0.14f;

            float cos_y = std::cos( m_seed_y * 0.1f + elapsed );
            float cos_y_start = std::cos( m_seed_y * 0.1f );
            float y_term = ( cos_y - cos_y_start ) * 0.04f;

            float angle = ( x_term - y_term + elapsed ) * ( spinner_rpm * 2.0f * omega_multiplier );

            float radius_x = 70.0f + 5.0f * std::sin( elapsed * 0.83f + m_seed_x * 1.4f )
                                   + 8.0f * std::sin( elapsed * 0.37f + m_seed_x )
                                   + 3.0f * std::sin( elapsed * 1.61f + m_seed_y * 3.0f );

            float radius_y = 70.0f + 5.0f * std::sin( elapsed * 0.71f + m_seed_y * 1.3f )
                                   + 8.0f * std::sin( elapsed * 0.29f + m_seed_y )
                                   + 3.0f * std::sin( elapsed * 1.4f  + m_seed_x * 0.9f );

            radius_x = std::clamp( radius_x, 50.0f, 90.0f );
            radius_y = std::clamp( radius_y, 50.0f, 90.0f );

            float center_offset_x = get_noise_2d( elapsed, m_seed_x + 300.f ) * 2.5f;
            float center_offset_y = get_noise_2d( elapsed, m_seed_y + 400.f ) * 2.5f;

            float px = cx + center_offset_x + std::cos( angle ) * radius_x;
            float py = cy + center_offset_y + std::sin( angle ) * radius_y;

            return { px, py };
        }

        int find_current_note( int gt, const osu::beatmap_data_t& map ) {
            if ( map.objects.empty( ) )
                return -1;

            for ( size_t i = 0; i < map.objects.size( ); ++i ) {
                const auto& obj = map.objects[ i ];
                int end_t;
                if ( is_spinner_object( obj ) ) {
                    end_t = obj.end_time;
                } else if ( obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) {
                    end_t = obj.end_time;
                } else {
                    int next_start = obj.start_time + 500;
                    if ( i + 1 < map.objects.size( ) )
                        next_start = map.objects[ i + 1 ].start_time;
                    end_t = obj.start_time + circle_hold_ms( obj.start_time, next_start ) + 8;
                }
                if ( gt >= obj.start_time && gt <= end_t )
                    return static_cast<int>( i );
            }
            return -1;
        }

        static float eval_cubic( float p0, float p1, float p2, float p3, float t ) {
            float mt = 1.f - t;
            return mt * mt * mt * p0
                 + 3.f * mt * mt * t * p1
                 + 3.f * mt * t * t * p2
                 + t * t * t * p3;
        }

        void compute_bezier_path( int gt, const osu::beatmap_data_t& map ) {
            if ( map.objects.empty( ) ) return;

            int prev_idx = -1;
            int next_idx = -1;
            for ( size_t i = 0; i < map.objects.size( ); ++i ) {
                if ( map.objects[i].start_time > gt ) {
                    next_idx = static_cast<int>( i );
                    if ( i > 0 ) prev_idx = static_cast<int>( i - 1 );
                    break;
                }
            }

            if ( next_idx < 0 ) return;

            float start_x = m_cursor_x >= 0.f ? m_cursor_x : ( prev_idx >= 0 ? map.objects[ prev_idx ].x : 256.f );
            float start_y = m_cursor_y >= 0.f ? m_cursor_y : ( prev_idx >= 0 ? map.objects[ prev_idx ].y : 192.f );

            const auto& next_obj = map.objects[ static_cast<size_t>( next_idx ) ];
            float end_x = next_obj.x;
            float end_y = next_obj.y;

            int start_time = gt;
            if ( prev_idx >= 0 ) {
                const auto& prev_obj = map.objects[ static_cast<size_t>( prev_idx ) ];
                start_time = get_movement_end_time( prev_obj, map, prev_idx );
                if ( start_time < gt ) start_time = gt;
            }
            int end_time = next_obj.start_time - 5;

            float dx = end_x - start_x;
            float dy = end_y - start_y;
            float dist = std::sqrt( dx * dx + dy * dy );

            float perp_x = 0.f, perp_y = 0.f;
            if ( dist > 0.001f ) {
                perp_x = -dy / dist;
                perp_y = dx / dist;
            }

            float offset_scale = curve_strength * dist * 0.5f;
            if ( dist < 50.f )
                offset_scale *= ( dist * 0.02f );

            float noise1 = get_noise_2d( static_cast<float>( gt ) * 0.01f, m_seed_x );
            float noise2 = get_noise_2d( static_cast<float>( gt ) * 0.01f, m_seed_y );

            m_bezier_cp0x = start_x;
            m_bezier_cp0y = start_y;
            m_bezier_cp1x = start_x + dx * 0.33f + perp_x * noise1 * offset_scale;
            m_bezier_cp1y = start_y + dy * 0.33f + perp_y * noise1 * offset_scale;
            m_bezier_cp2x = start_x + dx * 0.66f + perp_x * noise2 * offset_scale;
            m_bezier_cp2y = start_y + dy * 0.66f + perp_y * noise2 * offset_scale;
            m_bezier_cp3x = end_x;
            m_bezier_cp3y = end_y;
            m_bezier_start_time = start_time;
            m_bezier_end_time = end_time;

            LARGE_INTEGER count, freq;
            QueryPerformanceCounter(&count);
            QueryPerformanceFrequency(&freq);
            m_bezier_start_ticks = count.QuadPart;
            m_bezier_ticks_per_ms = static_cast<double>(freq.QuadPart) / 1000.0;

            m_bezier_valid = true;
        }

        point_t get_target_position_osu( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            const int gt = game.cur_time;
            if ( map.objects.empty( ) )
                return { 256.f, 192.f };

            if ( gt < map.objects[0].start_time ) {
                return { map.objects[0].x, map.objects[0].y };
            }

            int prev_idx = -1;
            int next_idx = -1;
            for ( size_t i = 0; i < map.objects.size( ); ++i ) {
                if ( map.objects[i].start_time > gt ) {
                    next_idx = static_cast<int>( i );
                    if ( i > 0 ) prev_idx = static_cast<int>( i - 1 );
                    break;
                }
            }

            if ( next_idx < 0 ) {
                const auto& last_obj = map.objects.back( );
                if ( last_obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) )
                    return evaluate_slider( last_obj, last_obj.end_time );
                return { last_obj.x, last_obj.y };
            }

            if ( prev_idx < 0 )
                return { map.objects[ next_idx ].x, map.objects[ next_idx ].y };

            const auto& prev_obj = map.objects[ static_cast<size_t>( prev_idx ) ];
            const auto& next_obj = map.objects[ static_cast<size_t>( next_idx ) ];
            int prev_end_t = get_movement_end_time( prev_obj, map, prev_idx );
            int gap = next_obj.start_time - prev_end_t;

            point_t base_pos{};
            bool is_bezier_eval = false;

            if ( m_bezier_valid && m_bezier_end_time > m_bezier_start_time ) {
                float duration = static_cast<float>( m_bezier_end_time - m_bezier_start_time );
                
                double elapsed_ms = 0.0;
                if ( m_bezier_ticks_per_ms > 0.0 ) {
                    LARGE_INTEGER count;
                    QueryPerformanceCounter(&count);
                    
                    float speed_multiplier = static_cast<float>( m_estimated_rate );
                    if ( speed_multiplier == 1.f ) {
                        if ( ( game.cur_mod_state & 64 ) != 0 || ( game.cur_mod_state & 512 ) != 0 ) {
                            speed_multiplier = 1.5f;
                        } else if ( ( game.cur_mod_state & 256 ) != 0 ) {
                            speed_multiplier = 0.75f;
                        }
                    }
                    
                    elapsed_ms = ( static_cast<double>( count.QuadPart - m_bezier_start_ticks ) / m_bezier_ticks_per_ms ) * speed_multiplier;
                } else {
                    elapsed_ms = static_cast<double>( gt - m_bezier_start_time );
                }

                float t = static_cast<float>( elapsed_ms / duration );
                t = std::clamp( t, 0.f, 1.f );

                bool is_stream = ( gap <= 148 );
                if ( !is_stream ) {
                    t = t * t * ( 3.f - 2.f * t );
                }

                base_pos.x = eval_cubic( m_bezier_cp0x, m_bezier_cp1x, m_bezier_cp2x, m_bezier_cp3x, t );
                base_pos.y = eval_cubic( m_bezier_cp0y, m_bezier_cp1y, m_bezier_cp2y, m_bezier_cp3y, t );
                is_bezier_eval = true;
            }

            if ( !is_bezier_eval ) {
                float duration = static_cast<float>( next_obj.start_time - prev_end_t );
                if ( duration <= 0.f ) {
                    base_pos = { next_obj.x, next_obj.y };
                } else {
                    float t = static_cast<float>( gt - prev_end_t ) / duration;
                    t = std::clamp( t, 0.f, 1.f );
                    t = t * t * ( 3.f - 2.f * t );

                    point_t start_raw = { prev_obj.x, prev_obj.y };
                    if ( prev_obj.type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) )
                        start_raw = evaluate_slider( prev_obj, prev_end_t );
                    else if ( is_spinner_object( prev_obj ) )
                        start_raw = get_spinner_position( prev_obj, prev_end_t );

                    base_pos = { start_raw.x + ( next_obj.x - start_raw.x ) * t,
                                 start_raw.y + ( next_obj.y - start_raw.y ) * t };
                }
            }

            if ( gap > 500 && gt > prev_end_t + 500 && drift_amount > 0.001f ) {
                float elapsed_idle = static_cast<float>( gt - (prev_end_t + 500) ) * 0.001f;
                float drift_scale = drift_amount * 6.0f;
                float noise_time = static_cast<float>( gt - prev_end_t );
                float noise_x = get_noise_2d( noise_time, m_seed_x );
                float noise_y = get_noise_2d( noise_time, m_seed_y );
                
                float ramp_factor = std::clamp( elapsed_idle, 0.f, 1.f );
                base_pos.x += noise_x * drift_scale * ramp_factor;
                base_pos.y += noise_y * drift_scale * ramp_factor;
            }

            return base_pos;
        }

        static point_t evaluate_bezier( const std::vector<point_t>& ctrl, float t ) {
            if ( ctrl.empty( ) ) return { 0.f, 0.f };
            std::vector<point_t> tmp = ctrl;
            size_t n = tmp.size( );
            for ( size_t step = 1; step < n; ++step ) {
                for ( size_t i = 0; i < n - step; ++i ) {
                    tmp[i].x = ( 1.f - t ) * tmp[i].x + t * tmp[i+1].x;
                    tmp[i].y = ( 1.f - t ) * tmp[i].y + t * tmp[i+1].y;
                }
            }
            return tmp[0];
        }

        static std::vector<point_t> generate_bezier_points( const std::vector<point_t>& ctrl ) {
            std::vector<point_t> result;
            if ( ctrl.empty( ) ) return result;

            std::vector<point_t> segment;
            for ( size_t i = 0; i < ctrl.size( ); ++i ) {
                segment.push_back( ctrl[i] );
                if ( i == ctrl.size( ) - 1 || ( std::abs( ctrl[i].x - ctrl[i+1].x ) < 0.01f && std::abs( ctrl[i].y - ctrl[i+1].y ) < 0.01f ) ) {
                    if ( segment.size( ) >= 2 ) {
                        const int num_samples = 32;
                        for ( int s = 0; s <= num_samples; ++s ) {
                            float t = static_cast<float>( s ) / static_cast<float>( num_samples );
                            result.push_back( evaluate_bezier( segment, t ) );
                        }
                    }
                    else if ( !segment.empty( ) ) {
                        result.push_back( segment[0] );
                    }
                    segment.clear( );
                }
            }
            return result;
        }

        static std::vector<point_t> generate_circle_points( const point_t& a, const point_t& b, const point_t& c ) {
            std::vector<point_t> result;
            float sideLength = (b.y - a.y) * (c.x - a.x) - (b.x - a.x) * (c.y - a.y);
            if (std::abs(sideLength) < 0.001f) {
                result.push_back(a);
                result.push_back(b);
                result.push_back(c);
                return result;
            }

            float d = 2.f * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
            if (std::abs(d) < 0.00001f) {
                result.push_back(a);
                result.push_back(b);
                result.push_back(c);
                return result;
            }

            float aSq = a.x * a.x + a.y * a.y;
            float bSq = b.x * b.x + b.y * b.y;
            float cSq = c.x * c.x + c.y * c.y;

            float ux = (aSq * (b.y - c.y) + bSq * (c.y - a.y) + cSq * (a.y - b.y)) / d;
            float uy = (aSq * (c.x - b.x) + bSq * (a.x - c.x) + cSq * (b.x - a.x)) / d;
            point_t center = { ux, uy };

            float dx_A = a.x - center.x;
            float dy_A = a.y - center.y;
            float dx_C = c.x - center.x;
            float dy_C = c.y - center.y;

            float radius = std::sqrt(dx_A * dx_A + dy_A * dy_A);

            float thetaStart = std::atan2(dy_A, dx_A);
            float thetaEnd = std::atan2(dy_C, dx_C);

            while (thetaEnd < thetaStart) {
                thetaEnd += 2.f * 3.141592653589793f;
            }

            float direction = 1.f;
            float thetaRange = thetaEnd - thetaStart;

            float ortho_x = c.y - a.y;
            float ortho_y = -(c.x - a.x);
            float dot = ortho_x * (b.x - a.x) + ortho_y * (b.y - a.y);
            if (dot < 0.f) {
                direction = -direction;
                thetaRange = 2.f * 3.141592653589793f - thetaRange;
            }

            int amountPoints = 2;
            float tolerance = 0.1f;
            if (2.f * radius > tolerance) {
                float angle = tolerance / radius;
                angle = 1.f - angle;
                angle = 2.f * std::acos(angle);

                float points = std::ceil(thetaRange / angle);
                if (std::isfinite(points)) {
                    amountPoints = std::max(2, static_cast<int>(points));
                }
            }

            result.reserve(amountPoints);
            for (int i = 0; i < amountPoints; ++i) {
                float fract = static_cast<float>(i) / static_cast<float>(amountPoints - 1);
                float theta = thetaStart + direction * fract * thetaRange;
                result.push_back({ center.x + std::cos(theta) * radius, center.y + std::sin(theta) * radius });
            }

            return result;
        }

        std::vector<point_t> generate_slider_path( const osu::hit_object_t& obj ) {
            std::vector<point_t> ctrl;
            ctrl.push_back( { obj.x, obj.y } );

            char type_char = 'L';
            if ( !obj.slider_curve_str.empty( ) ) {
                std::stringstream ss( obj.slider_curve_str );
                std::string item;
                bool is_first = true;
                while ( std::getline( ss, item, '|' ) ) {
                    if ( is_first ) {
                        if ( !item.empty( ) ) type_char = item[0];
                        is_first = false;
                        continue;
                    }
                    auto colon = item.find( ':' );
                    if ( colon != std::string::npos ) {
                        try {
                            float px = std::stof( item.substr( 0, colon ) );
                            float py = std::stof( item.substr( colon + 1 ) );
                            ctrl.push_back( { px, py } );
                        }
                        catch ( ... ) {}
                    }
                }
            }

            if ( ctrl.size( ) < 2 ) {
                return ctrl;
            }

            if ( type_char == 'P' && ctrl.size( ) == 3 ) {
                return generate_circle_points( ctrl[0], ctrl[1], ctrl[2] );
            }
            else if ( type_char == 'B' || type_char == 'P' || type_char == 'C' ) {
                return generate_bezier_points( ctrl );
            }

            return ctrl;
        }

        const cached_slider_t& get_slider_path( const osu::hit_object_t& obj ) {
            for ( const auto& c : m_slider_cache ) {
                if ( c.start_time == obj.start_time )
                    return c;
            }

            cached_slider_t cached;
            cached.start_time = obj.start_time;
            cached.path = generate_slider_path( obj );

            float total_dist = 0.f;
            cached.dists.push_back( 0.f );
            for ( size_t i = 1; i < cached.path.size( ); ++i ) {
                float dx = cached.path[i].x - cached.path[i-1].x;
                float dy = cached.path[i].y - cached.path[i-1].y;
                float d = std::sqrt( dx * dx + dy * dy );
                total_dist += d;
                cached.dists.push_back( total_dist );
            }
            cached.total_dist = total_dist;

            m_slider_cache.push_back( cached );
            return m_slider_cache.back( );
        }

        point_t evaluate_slider( const osu::hit_object_t& obj, int gt ) {
            const auto& cached = get_slider_path( obj );
            if ( cached.path.size( ) < 2 || cached.total_dist <= 0.f ) {
                return { obj.x, obj.y };
            }

            const float duration = static_cast<float>( obj.end_time - obj.start_time );
            if ( duration <= 0.f ) return { obj.x, obj.y };

            const float elapsed = static_cast<float>( gt - obj.start_time );
            float t = std::clamp( elapsed / duration, 0.f, 1.f );

            int repeats = obj.slider_repeat;
            if ( repeats < 1 ) repeats = 1;

            float repeat_prog = t * static_cast<float>( repeats );
            int current_lap = static_cast<int>( repeat_prog );
            float lap_t = repeat_prog - static_cast<float>( current_lap );
            if ( current_lap >= repeats ) {
                lap_t = ( repeats % 2 == 1 ) ? 1.f : 0.f;
            }
            else if ( current_lap % 2 == 1 ) {
                lap_t = 1.f - lap_t;
            }

            float target_dist = lap_t * cached.total_dist;
            point_t pos = cached.path.back( );
            for ( size_t i = 1; i < cached.path.size( ); ++i ) {
                if ( target_dist <= cached.dists[i] ) {
                    float segment_len = cached.dists[i] - cached.dists[i-1];
                    float seg_t = segment_len > 0.f ? ( target_dist - cached.dists[i-1] ) / segment_len : 0.f;
                    pos.x = cached.path[i-1].x + ( cached.path[i].x - cached.path[i-1].x ) * seg_t;
                    pos.y = cached.path[i-1].y + ( cached.path[i].y - cached.path[i-1].y ) * seg_t;
                    break;
                }
            }
            return pos;
        }
    };

}