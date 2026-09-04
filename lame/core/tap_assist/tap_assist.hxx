#pragma once

#include <impl/struct/game_snapshot.hxx>
#include <impl/memory/input.hxx>
#include <Windows.h>
#include <cstdint>
#include <cmath>
#include <climits>
#include <vector>
#include <cstring>
#include <algorithm>
#include <random>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

namespace tap_assist {

    class c_tap_assist {
    public:
        bool  enabled        = false;
        int   assist_window  = 125;
        int   randomization  = 15;
        bool  ignore_sliders = false;

        ~c_tap_assist( ) {
            stop_worker( );
        }

        void on_leave_play( const osu::game_snapshot_t& ) {
            std::lock_guard<std::recursive_mutex> lock( m_mutex );
            WORD k1 = 0, k2 = 0;
            resolve_keys_locked( k1, k2 );
            if ( m_k1_simulated_down ) {
                input::release_vk( k1 );
                m_k1_simulated_down = false;
            }
            if ( m_k2_simulated_down ) {
                input::release_vk( k2 );
                m_k2_simulated_down = false;
            }
            m_press_queue.clear( );
            m_release_queue.clear( );
            m_current_obj_index = 0;
            m_k1_physical_down = false;
            m_k2_physical_down = false;
            m_last_map = {};
            m_obj_skip_flag = false;
            m_obj_skip_index = 0;
            m_global_circle_flag = false;
            m_session_bias_generated = false;
            m_session_bias = 0.f;
            m_k1_is_corrected = false;
            m_k2_is_corrected = false;
            m_k1_shift = 0;
            m_k2_shift = 0;
            m_k1_scheduled_press_time = 0;
            m_k2_scheduled_press_time = 0;
            m_estimated_rate = 1.0;
            m_rate_ref_real_time = 0.0;
            m_rate_ref_game_time = 0;
        }

        bool handle_key_event( int vk, bool is_down, int64_t press_qpc = 0 ) {
            std::lock_guard<std::recursive_mutex> lock( m_mutex );

            if ( !enabled || m_last_game.cur_state != osu::game_state_t::play || !m_last_map.loaded || m_last_map.objects.empty( ) ) {
                return false;
            }

            WORD k1 = 0, k2 = 0;
            resolve_keys_locked( k1, k2 );

            if ( vk != k1 && vk != k2 ) {
                return false;
            }

            int game_time = get_interpolated_game_time( press_qpc );

            if ( is_down ) {
                if ( vk == k1 ) {
                    if ( m_k1_physical_down ) return true;
                    m_k1_physical_down = true;
                } else {
                    if ( m_k2_physical_down ) return true;
                    m_k2_physical_down = true;
                }

                float speed_mult = static_cast<float>( m_estimated_rate );

                float effective_od = m_last_map.od;
                if ( m_last_game.cur_mod_state & 16 )
                    effective_od = std::min( 10.f, effective_od * 1.4f );
                if ( m_last_game.cur_mod_state & 2 )
                    effective_od *= 0.5f;
                const float hit_window_300 = ( 80.f - 6.f * effective_od );
                const float max_diff_window = ( 200.f - 10.f * effective_od );

                const auto& obj_arr = m_last_map.objects;
                const int64_t object_count = static_cast<int64_t>( obj_arr.size( ) );
                int64_t obj_index = m_current_obj_index;

                if ( object_count == 0 || obj_index >= object_count ) {
                    return false;
                }

                const osu::hit_object_t* found_obj = nullptr;
                int64_t found_idx = -1;

                while ( obj_index < object_count ) {
                    if ( !m_obj_skip_flag || m_obj_skip_index != obj_index ) {
                        const auto& obj = obj_arr[ static_cast<size_t>( obj_index ) ];
                        const uint8_t obj_type = obj.type;

                        const bool is_slider = ( obj_type & static_cast<uint8_t>( osu::hit_object_type_t::slider ) ) != 0;
                        const bool is_spinner = ( obj_type & static_cast<uint8_t>( osu::hit_object_type_t::spinner ) ) != 0;

                        if ( is_spinner ) {

                        } else if ( is_slider && ignore_sliders ) {

                        } else {
                            found_obj = &obj;
                            found_idx = obj_index;
                            break;
                        }
                    }

                    ++obj_index;
                }

                if ( !found_obj ) {
                    return false;
                }

                const int diff = game_time - found_obj->start_time;

                if ( diff > static_cast<int>( max_diff_window ) ) {
                    m_current_obj_index = found_idx + 1;
                    return false;
                }

                if ( diff >= -static_cast<int>( hit_window_300 ) ) {
                    m_current_obj_index = found_idx + 1;
                    return false;
                }

                const float scaled_assist_window = static_cast<float>( assist_window ) * speed_mult;
                if ( diff < -static_cast<int>( scaled_assist_window ) )
                    return false;

                const int corrected = calculate_corrected_time( found_obj->start_time );
                m_press_queue.push_back( { corrected, vk } );

                if ( vk == k1 ) {
                    m_k1_is_corrected = true;
                    m_k1_shift = corrected - game_time;
                    m_k1_scheduled_press_time = corrected;
                } else {
                    m_k2_is_corrected = true;
                    m_k2_shift = corrected - game_time;
                    m_k2_scheduled_press_time = corrected;
                }
                m_current_obj_index = found_idx + 1;
                return true;

            } else {
                if ( vk == k1 )
                    m_k1_physical_down = false;
                else
                    m_k2_physical_down = false;

                bool was_corrected = false;
                int shift = 0;
                if ( vk == k1 ) {
                    was_corrected = m_k1_is_corrected;
                    shift = m_k1_shift;
                    m_k1_is_corrected = false;
                } else {
                    was_corrected = m_k2_is_corrected;
                    shift = m_k2_shift;
                    m_k2_is_corrected = false;
                }

                if ( was_corrected ) {
                    const int scheduled_press = ( vk == k1 ) ? m_k1_scheduled_press_time : m_k2_scheduled_press_time;
                    int corrected_release = game_time + shift;
                    if ( corrected_release < scheduled_press + 25 ) {
                        corrected_release = scheduled_press + 25;
                    }
                    m_release_queue.push_back( { corrected_release, vk } );
                    return true;
                }

                bool has_pending = false;
                for ( const auto& item : m_press_queue ) {
                    if ( item.vk == vk ) has_pending = true;
                }
                for ( const auto& item : m_release_queue ) {
                    if ( item.vk == vk ) has_pending = true;
                }

                if ( has_pending )
                    return true;

                if ( vk == k1 )
                    m_k1_simulated_down = false;
                else
                    m_k2_simulated_down = false;
                return false;
            }
        }

        void update( const osu::game_snapshot_t& game, const osu::beatmap_data_t& map ) {
            if ( enabled && !m_worker_running.load( ) ) {
                start_worker( );
            }

            {
                std::lock_guard<std::recursive_mutex> lock( m_mutex );
                m_last_game = game;
                if ( m_last_map.beatmap_path != map.beatmap_path || m_last_map.map_id != map.map_id ) {
                    m_last_map = map;
                }
            }

            if ( !enabled || game.cur_state != osu::game_state_t::play )
                return;

            const int game_time = game.cur_time;

            double real_now = 0.0;
            LARGE_INTEGER qpc_count, qpc_freq;
            if ( QueryPerformanceCounter( &qpc_count ) && QueryPerformanceFrequency( &qpc_freq ) ) {
                real_now = ( static_cast<double>( qpc_count.QuadPart ) / qpc_freq.QuadPart ) * 1000.0;
            }

            {
                std::lock_guard<std::recursive_mutex> lock( m_mutex );
                if ( m_rate_ref_real_time == 0.0 || game_time < m_rate_ref_game_time || game_time > m_rate_ref_game_time + 5000 ) {
                    m_rate_ref_real_time = real_now;
                    m_rate_ref_game_time = game_time;
                    m_estimated_rate = 1.0;
                } else {
                    double real_elapsed = real_now - m_rate_ref_real_time;
                    if ( real_elapsed >= 250.0 ) {
                        int game_elapsed = game_time - m_rate_ref_game_time;
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
                        m_rate_ref_game_time = game_time;
                    }
                }
            }
        }

    private:
        struct scheduled_input_t {
            int target_time = 0;
            int vk = 0;
        };

        std::recursive_mutex m_mutex;
        osu::game_snapshot_t m_last_game;
        osu::beatmap_data_t m_last_map;

        std::vector<scheduled_input_t> m_press_queue;
        std::vector<scheduled_input_t> m_release_queue;

        bool m_k1_physical_down = false;
        bool m_k2_physical_down = false;
        bool m_k1_simulated_down = false;
        bool m_k2_simulated_down = false;

        bool m_k1_is_corrected = false;
        bool m_k2_is_corrected = false;
        int  m_k1_shift = 0;
        int  m_k2_shift = 0;
        int  m_k1_scheduled_press_time = 0;
        int  m_k2_scheduled_press_time = 0;

        bool    m_obj_skip_flag = false;
        int64_t m_obj_skip_index = 0;
        bool    m_global_circle_flag = false;
        int64_t m_current_obj_index = 0;

        uint32_t m_minstd_seed{ std::random_device{ }( ) };

        bool  m_session_bias_generated = false;
        float m_session_bias = 0.f;

        std::thread m_worker_thread;
        std::atomic<bool> m_worker_running{ false };

        void start_worker( ) {
            if ( m_worker_running.load( ) )
                return;
            m_worker_running.store( true );
            m_worker_thread = std::thread( [this] { worker_loop( ); } );
        }

        void stop_worker( ) {
            if ( !m_worker_running.load( ) )
                return;
            m_worker_running.store( false );
            if ( m_worker_thread.joinable( ) )
                m_worker_thread.join( );
        }

        void worker_loop( ) {
            while ( m_worker_running.load( ) ) {
                if ( !enabled || m_last_game.cur_state != osu::game_state_t::play ) {
                    std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
                    continue;
                }

                int game_time = get_interpolated_game_time( 0 );

                WORD k1 = 0, k2 = 0;
                std::vector<std::pair<WORD, bool>> actions;
                {
                    std::lock_guard<std::recursive_mutex> lock( m_mutex );
                    resolve_keys_locked( k1, k2 );

                    for ( auto it = m_press_queue.begin( ); it != m_press_queue.end( ); ) {
                        if ( game_time >= it->target_time ) {
                            if ( it->vk == k1 ) {
                                if ( !m_k1_simulated_down ) {
                                    actions.emplace_back( k1, true );
                                    m_k1_simulated_down = true;
                                }
                            } else if ( it->vk == k2 ) {
                                if ( !m_k2_simulated_down ) {
                                    actions.emplace_back( k2, true );
                                    m_k2_simulated_down = true;
                                }
                            }
                            it = m_press_queue.erase( it );
                        } else {
                            ++it;
                        }
                    }

                    for ( auto it = m_release_queue.begin( ); it != m_release_queue.end( ); ) {
                        if ( game_time >= it->target_time ) {
                            if ( it->vk == k1 ) {
                                if ( m_k1_simulated_down ) {
                                    actions.emplace_back( k1, false );
                                    m_k1_simulated_down = false;
                                }
                            } else if ( it->vk == k2 ) {
                                if ( m_k2_simulated_down ) {
                                    actions.emplace_back( k2, false );
                                    m_k2_simulated_down = false;
                                }
                            }
                            it = m_release_queue.erase( it );
                        } else {
                            ++it;
                        }
                    }
                }

                for ( const auto& a : actions ) {
                    if ( a.second )
                        input::press_vk( a.first );
                    else
                        input::release_vk( a.first );
                }

                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            }
        }

        int get_interpolated_game_time( int64_t press_qpc = 0 ) {
            double press_real_time = 0.0;
            static LARGE_INTEGER freq;
            static bool freq_init = false;
            if ( !freq_init ) {
                QueryPerformanceFrequency( &freq );
                freq_init = true;
            }
            if ( press_qpc > 0 ) {
                press_real_time = static_cast<double>( press_qpc ) / static_cast<double>( freq.QuadPart ) * 1000.0;
            } else {
                LARGE_INTEGER now;
                QueryPerformanceCounter( &now );
                press_real_time = static_cast<double>( now.QuadPart ) / static_cast<double>( freq.QuadPart ) * 1000.0;
            }

            std::lock_guard<std::recursive_mutex> lock( m_mutex );
            double elapsed_real = press_real_time - m_rate_ref_real_time;
            if ( elapsed_real < 0.0 ) elapsed_real = 0.0;
            if ( elapsed_real > 1000.0 ) elapsed_real = 1000.0;

            return m_rate_ref_game_time + static_cast<int>( elapsed_real * m_estimated_rate );
        }

        uint32_t minstd_step( ) {
            m_minstd_seed = static_cast<uint32_t>( ( 48271ULL * m_minstd_seed ) % 0x7FFFFFFF );
            return m_minstd_seed;
        }

        void resolve_keys_locked( WORD& k1, WORD& k2 ) {
            k1 = static_cast<WORD>( m_last_game.left_key );
            k2 = static_cast<WORD>( m_last_game.right_key );
            if ( !k1 ) k1 = 'S';
            if ( !k2 ) k2 = 'D';
        }

        int calculate_corrected_time( int object_start_time ) {
            float speed_mult = static_cast<float>( m_estimated_rate );

            float effective_od = m_last_map.od;
            if ( m_last_game.cur_mod_state & 16 )
                effective_od = std::min( 10.f, effective_od * 1.4f );
            if ( m_last_game.cur_mod_state & 2 )
                effective_od *= 0.5f;
            const float hit_window_300 = ( 80.f - 6.f * effective_od );

            int truncated_window = static_cast<int>( hit_window_300 );
            if ( truncated_window != static_cast<int>( 0x80000000 ) && static_cast<float>( truncated_window ) != hit_window_300 ) {
                uint32_t raw;
                std::memcpy( &raw, &hit_window_300, sizeof( raw ) );
                if ( raw >> 31 )
                    truncated_window = static_cast<int>( hit_window_300 ) - 1;
            }

            const int base_time = object_start_time - truncated_window;

            if ( randomization <= 0 )
                return base_time;

            if ( !m_session_bias_generated ) {
                m_session_bias_generated = true;
                const float u = static_cast<float>( minstd_step( ) ) / 2147483647.f;
                m_session_bias = u * 0.3f;
            }

            const float hit_rand = static_cast<float>( minstd_step( ) ) / 2147483647.f;

            const float combined = m_session_bias + hit_rand * 0.7f;
            const float scaled_randomization = static_cast<float>( randomization ) * speed_mult;
            int int_result = static_cast<int>( combined * scaled_randomization );

            if ( int_result < 0 )
                int_result = 0;
            if ( int_result > static_cast<int>( scaled_randomization ) )
                int_result = static_cast<int>( scaled_randomization );

            int corrected_time = base_time + int_result;
            int max_possible = object_start_time - 1;
            if ( max_possible < corrected_time )
                corrected_time = max_possible;

            return corrected_time;
        }

        double m_estimated_rate = 1.0;
        double m_rate_ref_real_time = 0.0;
        int m_rate_ref_game_time = 0;
    };

}