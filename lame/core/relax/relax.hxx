#pragma once

#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <core/relax/nt_input.hxx>
#include <Windows.h>
#include <vector>
#include <algorithm>
#include <random>
#include <cmath>
#include <chrono>
#include <mutex>

namespace relax {

    enum class tap_style_t : int {
        alternate = 0,
        singletap = 1
    };

    class c_relax {
    public:
        bool enabled = false;

        float ur = 65.f;

        int tap_style = static_cast<int>( tap_style_t::alternate );

        int singletap_bpm_cap = 100;

        float k1_hold_center = 48.f;
        float k1_hold_spread = 12.f;

        float k2_hold_center = 48.f;
        float k2_hold_spread = 12.f;

        float hold_floor = 15.f;
        float hold_ceiling = 115.f;

        int32_t manual_offset_ms = 0;

        [[nodiscard]] size_t queue_size( ) const {
            std::lock_guard lock( m_mtx );
            return m_click_queue.size( );
        }
        [[nodiscard]] bool is_synced( ) const { return true; }
        [[nodiscard]] int last_hit_obj_idx( ) const {
            std::lock_guard lock( m_mtx );
            return m_last_hit_obj_idx;
        }

        [[nodiscard]] bool is_active( ) const {
            std::lock_guard lock( m_mtx );
            return enabled && m_in_play && !m_click_queue.empty( );
        }

        void on_leave_play( const osu::game_snapshot_t& game ) {
            std::lock_guard lock( m_mtx );
            leave_play( game );
        }

    private:
        void leave_play( const osu::game_snapshot_t& game ) {
            release_all_keys( game );
            m_click_queue.clear( );
            m_last_hit_obj_idx = -1;
            m_scheduled_through_idx = -1;
            m_last_click_time = -99999;
            m_in_play = false;
            m_use_k2_next = false;
            m_last_audio_time = 0;
            m_last_audio_sync_time = 0.0;
            m_last_jitter = 0.f;
        }

    public:

        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            std::lock_guard lock( m_mtx );
            if ( !enabled || game.cur_state != osu::game_state_t::play || !map.loaded || map.objects.empty( ) ) {
                if ( m_in_play )
                    leave_play( game );
                return;
            }

            m_in_play = true;
            const int game_time = game.cur_time;

            if ( game_time < m_last_game_time - 200 ) {
                reset_state( game );
            }

            schedule_clicks( game, map );

            m_last_game_time = game_time;

            advance_past_objects( game, map );
            purge_stale( game_time );
            flush_queue( game );
        }

    private:
        struct scheduled_click_t {
            int press_time = 0;
            int release_time = 0;
            WORD key = 0;
            bool pressed = false;
            bool released = false;
        };

        std::vector<scheduled_click_t> m_click_queue;
        bool m_in_play = false;
        int m_last_game_time = 0;
        int m_last_hit_obj_idx = -1;
        int m_scheduled_through_idx = -1;
        int m_last_click_time = -99999;
        bool m_use_k2_next = false;
        bool m_left_down = false;
        bool m_right_down = false;
        int m_last_audio_time = 0;
        double m_last_audio_sync_time = 0.0;
        float m_last_jitter = 0.f;

        c_nt_input m_nt;

        mutable std::mutex m_mtx;

        std::mt19937 m_rng{ []() -> uint32_t {
            try { return std::random_device{}( ); }
            catch ( ... ) { return static_cast<uint32_t>( std::chrono::high_resolution_clock::now( ).time_since_epoch( ).count( ) ); }
        }( ) };
        std::normal_distribution<float> m_norm{ 0.f, 1.f };
        std::uniform_real_distribution<float> m_unit{ 0.f, 1.f };
        std::uniform_real_distribution<float> m_sub_ms{ -1.49f, 1.49f };

        float generate_hold( float center, float spread ) {
            const float z = m_norm( m_rng );

            const float skew = 0.3f;
            const float shaped = z + skew * ( z * z - 1.f );

            float hold = center + shaped * spread;
            hold += ( m_unit( m_rng ) - 0.5f ) * 2.f;

            return std::clamp( hold, hold_floor, hold_ceiling );
        }

        float hit_jitter( const osu::beatmap_data_t& map ) {
            const float sigma = ur / 10.f;
            float new_jitter = m_norm( m_rng ) * ( sigma * 2.2f ) + m_sub_ms( m_rng );
            float jitter = 0.6f * m_last_jitter + 0.4f * new_jitter;
            m_last_jitter = jitter;

            float actual_od = map.od;
            if ( map.hr ) actual_od = std::min( 10.f, actual_od * 1.4f );
            if ( map.ez ) actual_od = actual_od * 0.5f;

            const float hit_window_300 = 79.5f - 6.f * actual_od;
            const float max_deviation = std::max( 5.f, hit_window_300 - 1.f );

            return std::clamp( jitter, -max_deviation, max_deviation );
        }

        static float bpm_from_interval( int interval_ms ) {
            return interval_ms > 0 ? 60000.f / static_cast<float>( interval_ms ) : 9999.f;
        }

        static void resolve_keys( const osu::game_snapshot_t& game, WORD& k1, WORD& k2 ) {
            k1 = static_cast<WORD>( game.left_key );
            k2 = static_cast<WORD>( game.right_key );
            if ( !k1 ) k1 = 'Z';
            if ( !k2 ) k2 = 'X';
        }

        void reset_state( const osu::game_snapshot_t& game ) {
            release_all_keys( game );
            m_click_queue.clear( );
            m_last_hit_obj_idx = -1;
            m_scheduled_through_idx = -1;
            m_last_click_time = -99999;
            m_use_k2_next = false;
            m_last_audio_time = 0;
            m_last_audio_sync_time = 0.0;
            m_last_jitter = 0.f;
        }

        void release_all_keys( const osu::game_snapshot_t& game ) {
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );
            const bool use_nt = m_nt.available( );
            if ( m_left_down ) {
                if ( use_nt ) m_nt.release( k1 );
                else input::release_vk( k1 );
                m_left_down = false;
            }
            if ( m_right_down ) {
                if ( use_nt ) m_nt.release( k2 );
                else input::release_vk( k2 );
                m_right_down = false;
            }
        }

        void press_key( WORD vk, bool& down ) {
            if ( !vk || down ) return;
            if ( m_nt.available( ) ) m_nt.press( vk );
            else input::press_vk( vk );
            down = true;
        }

        void release_key( WORD vk, bool& down ) {
            if ( !vk || !down ) return;
            if ( m_nt.available( ) ) m_nt.release( vk );
            else input::release_vk( vk );
            down = false;
        }

        bool should_alternate( int inter_tap_ms ) {
            if ( static_cast<tap_style_t>( tap_style ) == tap_style_t::alternate )
                return true;

            return bpm_from_interval( inter_tap_ms ) >= static_cast<float>( singletap_bpm_cap );
        }

        void advance_past_objects( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            const int gt = game.cur_time;
            while ( m_last_hit_obj_idx + 1 < static_cast<int>( map.objects.size( ) ) ) {
                const auto& obj = map.objects[ static_cast<size_t>( m_last_hit_obj_idx + 1 ) ];
                if ( obj.start_time < gt - 50 )
                    m_last_hit_obj_idx++;
                else
                    break;
            }
        }

        void purge_stale( int game_time ) {
            m_click_queue.erase(
                std::remove_if( m_click_queue.begin( ), m_click_queue.end( ),
                    [ game_time ]( const scheduled_click_t& c ) {
                        return !c.pressed && c.press_time < game_time - 50;
                    } ),
                m_click_queue.end( ) );
        }

        void schedule_clicks( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );

            int current_stream_length = 0;
            int last_obj_start_time = -99999;
            if ( m_scheduled_through_idx >= 0 && m_scheduled_through_idx < static_cast<int>( map.objects.size( ) ) ) {
                last_obj_start_time = map.objects[ static_cast<size_t>( m_scheduled_through_idx ) ].start_time;
            }

            for ( int i = m_scheduled_through_idx + 1; i < static_cast<int>( map.objects.size( ) ); ++i ) {
                const auto& obj = map.objects[ static_cast<size_t>( i ) ];

                const int prev_interval = obj.start_time - last_obj_start_time;
                if ( prev_interval > 0 && prev_interval < 100 ) {
                    current_stream_length++;
                }
                else {
                    current_stream_length = 0;
                }
                last_obj_start_time = obj.start_time;
                const float jitter_ms = hit_jitter( map );
                const int press_time = obj.start_time + static_cast<int>( std::round( jitter_ms ) ) + manual_offset_ms;

                const bool is_slider = ( obj.type & 2 ) != 0;
                const bool is_spinner = ( obj.type & 8 ) != 0;
                const int slider_dur = obj.end_time - obj.start_time;
                const int natural_hold = ( ( is_slider && slider_dur >= 120 ) || is_spinner ) ? slider_dur : 0;
                int hold_dur = 0;

                int next_interval = 9999;
                if ( i + 1 < static_cast<int>( map.objects.size( ) ) ) {
                    next_interval = map.objects[ static_cast<size_t>( i + 1 ) ].start_time - obj.start_time;
                    if ( next_interval < 0 ) next_interval = 0;
                }

                if ( natural_hold > 0 ) {
                    const float tail = ( m_unit( m_rng ) * 2.f - 1.f ) * 5.f + m_sub_ms( m_rng );
                    hold_dur = natural_hold + static_cast<int>( tail );
                    if ( hold_dur < 15 ) hold_dur = 15;
                }
                else {
                    float center = m_use_k2_next ? k2_hold_center : k1_hold_center;
                    float spread = m_use_k2_next ? k2_hold_spread : k1_hold_spread;

                    if ( next_interval < 250 ) {
                        float max_allowed_center = static_cast<float>( next_interval ) * 0.52f;
                        float dynamic_floor = 34.f + m_unit( m_rng ) * 6.f;
                        if ( max_allowed_center < dynamic_floor ) {
                            max_allowed_center = dynamic_floor;
                        }
                        if ( max_allowed_center < center ) {
                            spread = spread * std::sqrt( max_allowed_center / center );
                            center = max_allowed_center;
                        }
                    }
                    float map_drift = ( static_cast<float>( i ) / static_cast<float>( map.objects.size( ) ) ) * 7.f;
                    float stream_drift = std::min( static_cast<float>( current_stream_length ) * 1.3f, 20.f );
                    float total_drift = map_drift + stream_drift;

                    center += total_drift;
                    float safe_cap = static_cast<float>( next_interval ) * 0.85f;
                    if ( center > safe_cap ) {
                        center = safe_cap;
                    }

                    hold_dur = static_cast<int>( generate_hold( center, spread ) );
                }

                if ( is_slider ) {
                    const int needed_hold = slider_dur - static_cast<int>( std::round( jitter_ms ) ) - manual_offset_ms;
                    const int floor_hold = slider_dur - 30;
                    if ( hold_dur < floor_hold ) hold_dur = floor_hold;
                    if ( hold_dur < needed_hold ) hold_dur = needed_hold;
                }

                const int release_time = press_time + hold_dur;

                const int inter_tap = press_time - m_last_click_time;
                if ( should_alternate( inter_tap ) )
                    m_use_k2_next = !m_use_k2_next;

                const WORD chosen = m_use_k2_next ? k2 : k1;
                if ( !chosen ) continue;

                m_click_queue.push_back( { press_time, release_time, chosen, false, false } );
                m_last_click_time = press_time;
                m_scheduled_through_idx = i;
            }
        }

        inline double get_time_ms( ) {
            static const auto start = std::chrono::high_resolution_clock::now( );
            return std::chrono::duration<double, std::milli>( std::chrono::high_resolution_clock::now( ) - start ).count( );
        }

        double get_interpolated_game_time( const osu::game_snapshot_t& game ) {
            const double now = get_time_ms( );
            const int gt = game.cur_time;
            
            if ( gt != m_last_audio_time ) {
                m_last_audio_time = gt;
                m_last_audio_sync_time = now;
            }
            
            double elapsed = now - m_last_audio_sync_time;
            const double speed = game.speed_mult > 0.01f ? game.speed_mult : 1.f;
            
            if ( elapsed > 30.0 / speed ) {
                elapsed = 0.0;
            }
            
            return static_cast<double>( gt ) + elapsed * speed;
        }

        void flush_queue( const osu::game_snapshot_t& game ) {
            const double est_gt = get_interpolated_game_time( game );
            WORD k1 = 0, k2 = 0;
            resolve_keys( game, k1, k2 );

            for ( auto& c : m_click_queue ) {
                if ( !c.pressed && est_gt >= static_cast<double>( c.press_time ) ) {
                    bool& ref = ( c.key == k1 ) ? m_left_down : m_right_down;
                    press_key( c.key, ref );
                    c.pressed = true;
                    continue;
                }
                if ( c.pressed && !c.released && est_gt >= static_cast<double>( c.release_time ) ) {
                    bool& ref = ( c.key == k1 ) ? m_left_down : m_right_down;
                    release_key( c.key, ref );
                    c.released = true;
                }
            }

            m_click_queue.erase(
                std::remove_if( m_click_queue.begin( ), m_click_queue.end( ),
                    []( const scheduled_click_t& c ) { return c.released; } ),
                m_click_queue.end( ) );
        }
    };

}