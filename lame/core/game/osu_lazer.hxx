#pragma once

#include <core/game/i_osu_client.hxx>
#include <impl/defs/offsets_lazer.hxx>
#include <impl/memory/scanner.hxx>
#include <impl/util/debug_log.hxx>
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <vector>
#include <cmath>
#include <array>
#include <unordered_map>
#include <cctype>
#include <cstring>
#include <utility>

namespace game {

    struct resolved_mod_tokens_t {
        uint32_t easy = 0;
        uint32_t hidden = 0;
        uint32_t hardrock = 0;
        uint32_t doubletime = 0;
        uint32_t halftime = 0;
        uint32_t nightcore = 0;
        uint32_t flashlight = 0;
        bool resolved = false;
    };

    class c_osu_lazer : public i_osu_client {
    public:
        explicit c_osu_lazer( offsets::lazer::table_t& offsets ) : m_off( offsets ) {}

        bool attach( memory::c_process& process ) override {
            dbg::log( "BUILD layout_exact_v5_structural" );
            const wchar_t* mods[] = {
                L"osu.Game.dll",
                L"osu.Framework.dll",
                L"osu.Game.Rulesets.Osu.dll",
                L"osu.Game.Rulesets.dll",
            };
            for ( auto mod_name : mods ) {
                if ( scan_module_for_game( process, mod_name ) ) {
                    dbg::log( "BASE attached module base=0x%llX root=0x%llX",
                        static_cast<unsigned long long>( m_game_base ),
                        static_cast<unsigned long long>( m_game_root_slot ) );
                    return true;
                }
            }

            if ( scan_private_memory_for_game( process ) ) {
                dbg::log( "BASE attached private base=0x%llX root=0x%llX",
                    static_cast<unsigned long long>( m_game_base ),
                    static_cast<unsigned long long>( m_game_root_slot ) );
                return true;
            }

            return false;
        }

        static uint64_t get_module_base( int32_t pid, const wchar_t* name ) {
            HANDLE snap = CreateToolhelp32Snapshot( TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, static_cast<DWORD>( pid ) );
            if ( snap == INVALID_HANDLE_VALUE )
                return 0;

            MODULEENTRY32W entry{};
            entry.dwSize = sizeof( entry );

            uint64_t base = 0;
            if ( Module32FirstW( snap, &entry ) ) {
                do {
                    if ( _wcsicmp( entry.szModule, name ) == 0 ) {
                        base = reinterpret_cast<uint64_t>( entry.modBaseAddr );
                        break;
                    }
                } while ( Module32NextW( snap, &entry ) );
            }

            CloseHandle( snap );
            return base;
        }

        void resolve_tokens( memory::c_process& process ) {
            if ( m_tokens.resolved )
                return;

            const uint64_t dll_base = get_module_base( process.pid( ), L"osu.Game.Rulesets.Osu.dll" );
            if ( !dll_base )
                return;

            const auto magic = process.read<uint16_t>( dll_base );
            if ( magic != 0x5a4d )
                return;

            const auto pe_offset = process.read<uint32_t>( dll_base + 0x3c );
            const auto pe_sig = process.read<uint32_t>( dll_base + pe_offset );
            if ( pe_sig != 0x00004550 )
                return;

            const auto opt_magic = process.read<uint16_t>( dll_base + pe_offset + 24 );
            const bool is_64bit = opt_magic == 0x20b;

            const auto data_dir_offset = pe_offset + 24 + ( is_64bit ? 112 : 96 );

            const auto cli_rva = process.read<uint32_t>( dll_base + data_dir_offset + 14 * 8 );
            if ( cli_rva == 0 )
                return;

            const auto cli_header_addr = dll_base + cli_rva;

            const auto meta_rva = process.read<uint32_t>( cli_header_addr + 8 );
            const auto meta_addr = dll_base + meta_rva;

            char bsjb[4] = { 0 };
            process.read_buffer( meta_addr, bsjb, 4 );
            if ( bsjb[0] != 'B' || bsjb[1] != 'S' || bsjb[2] != 'J' || bsjb[3] != 'B' )
                return;

            const auto version_len = process.read<uint32_t>( meta_addr + 12 );
            const auto streams_offset = 16 + version_len;
            const auto aligned_streams_offset = ( streams_offset + 3 ) & ~3;

            const auto num_streams = process.read<uint16_t>( meta_addr + aligned_streams_offset + 2 );

            uint64_t curr_stream_header = meta_addr + aligned_streams_offset + 4;
            uint64_t table_stream_addr = 0;
            uint64_t string_stream_addr = 0;

            for ( uint16_t i = 0; i < num_streams; ++i ) {
                const auto offset = process.read<uint32_t>( curr_stream_header );

                char name_buf[32] = { 0 };
                process.read_buffer( curr_stream_header + 8, name_buf, sizeof( name_buf ) - 1 );
                std::string name( name_buf );

                if ( name == "#~" || name == "#-" ) {
                    table_stream_addr = meta_addr + offset;
                } else if ( name == "#Strings" ) {
                    string_stream_addr = meta_addr + offset;
                }

                const auto name_len = name.length( );
                const auto total_len = 8 + name_len + 1;
                const auto aligned_total_len = ( total_len + 3 ) & ~3;
                curr_stream_header += aligned_total_len;
            }

            if ( table_stream_addr == 0 || string_stream_addr == 0 )
                return;

            const auto heap_sizes = process.read<uint8_t>( table_stream_addr + 6 );
            const auto valid_mask = process.read<uint64_t>( table_stream_addr + 8 );

            const auto string_idx_size = ( heap_sizes & 0x01 ) ? 4 : 2;
            const auto guid_idx_size = ( heap_sizes & 0x02 ) ? 4 : 2;

            uint32_t row_counts[64] = { 0 };
            uint64_t curr_ptr = table_stream_addr + 24;
            for ( int i = 0; i < 64; ++i ) {
                if ( ( valid_mask >> i ) & 1 ) {
                    row_counts[i] = process.read<uint32_t>( curr_ptr );
                    curr_ptr += 4;
                }
            }

            uint64_t table_data_ptr = curr_ptr;

            table_data_ptr += row_counts[0x00] * ( 2 + string_idx_size + 3 * guid_idx_size );

            const auto max_scope_rows = (std::max)( { row_counts[0x00], row_counts[0x1a], row_counts[0x23], row_counts[0x01] } );
            const auto resolution_scope_size = ( max_scope_rows < 16384 ) ? 2 : 4;
            table_data_ptr += row_counts[0x01] * ( resolution_scope_size + 2 * string_idx_size );

            const auto max_tdr_rows = (std::max)( { row_counts[0x02], row_counts[0x01], row_counts[0x1b] } );
            const auto typedef_or_ref_size = ( max_tdr_rows < 16384 ) ? 2 : 4;
            const auto field_idx_size = ( row_counts[0x04] < 65536 ) ? 2 : 4;
            const auto method_idx_size = ( row_counts[0x06] < 65536 ) ? 2 : 4;
            const auto typedef_row_size = 4 + 2 * string_idx_size + typedef_or_ref_size + field_idx_size + method_idx_size;

            const auto num_typedefs = row_counts[0x02];
            for ( uint32_t idx = 0; idx < num_typedefs; ++idx ) {
                const auto row_addr = table_data_ptr + idx * typedef_row_size;
                uint32_t name_idx = 0;
                if ( string_idx_size == 2 ) {
                    name_idx = process.read<uint16_t>( row_addr + 4 );
                } else {
                    name_idx = process.read<uint32_t>( row_addr + 4 );
                }

                char name_buf[64] = { 0 };
                process.read_buffer( string_stream_addr + name_idx, name_buf, sizeof( name_buf ) - 1 );
                std::string name( name_buf );

                const uint32_t token = 0x02000000 | ( idx + 1 );
                if ( name == "OsuModEasy" ) {
                    m_tokens.easy = token;
                } else if ( name == "OsuModHidden" ) {
                    m_tokens.hidden = token;
                } else if ( name == "OsuModHardRock" ) {
                    m_tokens.hardrock = token;
                } else if ( name == "OsuModDoubleTime" ) {
                    m_tokens.doubletime = token;
                } else if ( name == "OsuModHalfTime" ) {
                    m_tokens.halftime = token;
                } else if ( name == "OsuModNightcore" ) {
                    m_tokens.nightcore = token;
                } else if ( name == "OsuModFlashlight" ) {
                    m_tokens.flashlight = token;
                }
            }

            m_tokens.resolved = true;
        }

        int32_t read_lazer_mods( memory::c_process& process ) {
            int32_t mods = 0;
            const auto selected_mods_bindable = process.read<uint64_t>( m_game_base + m_off.game_base_selected_mods );
            if ( !selected_mods_bindable )
                return mods;

            const auto selected_mods_list = process.read<uint64_t>( selected_mods_bindable + m_off.bindable_value );
            if ( !selected_mods_list )
                return mods;

            const auto items = process.read<uint64_t>( selected_mods_list + m_off.list_items );
            const auto size = process.read<int32_t>( selected_mods_list + m_off.list_size );
            if ( !items || size <= 0 || size > 32 )
                return mods;

            resolve_tokens( process );
            if ( !m_tokens.resolved )
                return mods;

            for ( int32_t i = 0; i < size; ++i ) {
                const auto mod_ptr = process.read<uint64_t>( items + m_off.array_first_element + static_cast<uint64_t>( i ) * 8 );
                if ( !mod_ptr )
                    continue;

                const auto mt = process.read<uint64_t>( mod_ptr );
                if ( !mt )
                    continue;

                const auto token_rid = process.read<uint16_t>( mt + 0xA );
                const uint32_t token = 0x02000000 | token_rid;

                if ( token == m_tokens.easy ) {
                    mods |= 2;
                } else if ( token == m_tokens.hidden ) {
                    mods |= 8;
                } else if ( token == m_tokens.hardrock ) {
                    mods |= 16;
                } else if ( token == m_tokens.doubletime ) {
                    mods |= 64;
                } else if ( token == m_tokens.halftime ) {
                    mods |= 256;
                } else if ( token == m_tokens.nightcore ) {
                    mods |= 512;
                } else if ( token == m_tokens.flashlight ) {
                    mods |= 1024;
                }
            }
            return mods;
        }

        int32_t read_lazer_mods_from_gameplay_state( memory::c_process& process, uint64_t gameplay_state ) {
            if ( !plausible_ptr( gameplay_state ) )
                return -1;

            const uint64_t mods_obj = process.read<uint64_t>(
                gameplay_state + m_off.gameplay_state_mods );
            if ( !plausible_ptr( mods_obj ) )
                return -1;

            resolve_tokens( process );
            if ( !m_tokens.resolved )
                return -1;

            uint64_t element_base = 0;
            int32_t count = process.read<int32_t>( mods_obj + 0x08 );

            // GameplayState normally receives the Mod[] created by Player. CLR
            // reference arrays use length +0x08 and first element +0x10.
            if ( count >= 0 && count <= 64 ) {
                element_base = mods_obj + 0x10;
            } else {
                const auto seq = resolve_sequence_layout( process, mods_obj, 64 );
                if ( !seq.ok )
                    return -1;
                count = seq.count;
                element_base = seq.items + seq.element_offset;
            }

            int32_t mods = 0;
            for ( int32_t i = 0; i < count; ++i ) {
                const uint64_t mod_ptr = process.read<uint64_t>(
                    element_base + 8ull * static_cast<uint64_t>( i ) );
                if ( !plausible_ptr( mod_ptr ) )
                    continue;

                const uint64_t mt = process.read<uint64_t>( mod_ptr );
                if ( !plausible_ptr( mt ) )
                    continue;

                const uint16_t token_rid = process.read<uint16_t>( mt + 0xA );
                const uint32_t token = 0x02000000 | token_rid;
                if ( token == m_tokens.easy ) mods |= 2;
                else if ( token == m_tokens.hidden ) mods |= 8;
                else if ( token == m_tokens.hardrock ) mods |= 16;
                else if ( token == m_tokens.doubletime ) mods |= 64;
                else if ( token == m_tokens.halftime ) mods |= 256;
                else if ( token == m_tokens.nightcore ) mods |= 512;
                else if ( token == m_tokens.flashlight ) mods |= 1024;
            }
            return mods;
        }

        static bool plausible_ptr( uint64_t p ) {
            return p >= 0x10000 && p < 0x7FFFFFFFFFFF && p != 0xFFFFFFFFFFFFFFFF;
        }

        void probe_clock_field( memory::c_process& process, uint64_t final_source, osu::game_snapshot_t& snap ) {
            if ( !plausible_ptr( final_source ) )
                return;

            int best_index = -1;
            int best_hits = 0;
            double best_value = 0.0;

            // Probe the first 0x100 bytes of the clock source for a double which
            // advances like a millisecond clock. This is diagnostic only.
            for ( int index = 0; index < static_cast<int>( m_clock_probe_prev.size( ) ); ++index ) {
                const uint32_t off = 0x08u + static_cast<uint32_t>( index ) * 8u;
                const double value = process.read<double>( final_source + off );
                if ( !std::isfinite( value ) || value < -60000.0 || value > 86400000.0 ) {
                    m_clock_probe_hits[ index ] = 0;
                    m_clock_probe_prev[ index ] = value;
                    continue;
                }

                const double prev = m_clock_probe_prev[ index ];
                if ( std::isfinite( prev ) && prev >= -60000.0 && prev <= 86400000.0 ) {
                    const double delta = value - prev;
                    if ( delta > 0.05 && delta < 100.0 ) {
                        if ( m_clock_probe_hits[ index ] < 100000 )
                            ++m_clock_probe_hits[ index ];
                    } else if ( delta < -1000.0 || delta > 5000.0 ) {
                        m_clock_probe_hits[ index ] = 0;
                    }
                }

                m_clock_probe_prev[ index ] = value;
                if ( m_clock_probe_hits[ index ] > best_hits ) {
                    best_hits = m_clock_probe_hits[ index ];
                    best_index = index;
                    best_value = value;
                }
            }

            if ( best_index >= 0 && best_hits >= 3 ) {
                snap.diag_probe_clock_offset = 0x08 + best_index * 8;
                snap.diag_probe_clock_hits = best_hits;
                snap.diag_probe_clock_value = static_cast<int32_t>( best_value );

                if ( best_hits >= m_last_clock_probe_hits ) {
                    const bool changed = m_last_clock_probe_offset != snap.diag_probe_clock_offset;
                    const bool milestone = best_hits == 3 || ( best_hits > m_last_clock_probe_hits && ( best_hits % 20 ) == 0 );
                    m_last_clock_probe_offset = snap.diag_probe_clock_offset;
                    m_last_clock_probe_hits = best_hits;
                    m_last_clock_probe_value = snap.diag_probe_clock_value;
                    m_last_probe_seen_ms = GetTickCount64( );
                    if ( changed || milestone )
                        dbg::log( "CLOCK candidate off=0x%X hits=%d value=%d", m_last_clock_probe_offset, m_last_clock_probe_hits, m_last_clock_probe_value );
                }
            }
        }

        void probe_clock_chain( memory::c_process& process, uint64_t beatmap_clock, osu::game_snapshot_t& snap ) {
            if ( !plausible_ptr( beatmap_clock ) )
                return;

            // This is deliberately throttled. It is a diagnostic scanner, not a
            // gameplay path. We scan pointer fields in BeatmapClock and bulk-read
            // the first 0x208 bytes of each pointed-to object, looking for a
            // double which advances like a millisecond clock over several samples.
            const ULONGLONG now = GetTickCount64( );
            if ( now - m_last_clock_chain_probe_ms >= 50 ) {
                m_last_clock_chain_probe_ms = now;

                int best_hits = m_clock_chain_best_hits;
                int best_src_off = m_clock_chain_best_source_offset;
                int best_time_off = m_clock_chain_best_time_offset;
                double best_value = static_cast<double>( m_clock_chain_best_value );

                constexpr int source_slots = 256; // +0x08 .. +0x800
                constexpr int time_slots = 64;    // +0x08 .. +0x200
                std::array<uint8_t, 0x208> object_buf{};

                for ( int si = 0; si < source_slots; ++si ) {
                    const uint32_t src_off = 0x08u + static_cast<uint32_t>( si ) * 8u;
                    const uint64_t source = process.read<uint64_t>( beatmap_clock + src_off );
                    if ( !plausible_ptr( source ) )
                        continue;

                    if ( !process.read_buffer( source, object_buf.data( ), object_buf.size( ) ) )
                        continue;

                    for ( int ti = 0; ti < time_slots; ++ti ) {
                        const uint32_t time_off = 0x08u + static_cast<uint32_t>( ti ) * 8u;
                        double value = 0.0;
                        std::memcpy( &value, object_buf.data( ) + time_off, sizeof( value ) );
                        if ( !std::isfinite( value ) || value < -60000.0 || value > 86400000.0 )
                            continue;

                        const size_t idx = static_cast<size_t>( si ) * time_slots + static_cast<size_t>( ti );
                        const double prev = m_clock_chain_prev[ idx ];
                        int& hits = m_clock_chain_hits[ idx ];

                        if ( std::isfinite( prev ) && prev >= -60000.0 && prev <= 86400000.0 ) {
                            const double delta = value - prev;
                            if ( delta > 0.05 && delta < 250.0 ) {
                                if ( hits < 100000 ) ++hits;
                            } else if ( delta < -1000.0 || delta > 5000.0 ) {
                                hits = 0;
                            }
                        }
                        m_clock_chain_prev[ idx ] = value;

                        if ( hits > best_hits ) {
                            best_hits = hits;
                            best_src_off = static_cast<int>( src_off );
                            best_time_off = static_cast<int>( time_off );
                            best_value = value;
                        }
                    }
                }

                if ( best_hits >= 3 ) {
                    const bool improved = best_hits > m_clock_chain_best_hits ||
                        best_src_off != m_clock_chain_best_source_offset ||
                        best_time_off != m_clock_chain_best_time_offset;
                    m_clock_chain_best_hits = best_hits;
                    m_clock_chain_best_source_offset = best_src_off;
                    m_clock_chain_best_time_offset = best_time_off;
                    m_clock_chain_best_value = static_cast<int32_t>( best_value );
                    if ( improved ) {
                        m_last_probe_seen_ms = GetTickCount64( );
                        dbg::log( "CLOCK_CHAIN candidate src=0x%X time=0x%X hits=%d value=%d", m_clock_chain_best_source_offset, m_clock_chain_best_time_offset, m_clock_chain_best_hits, m_clock_chain_best_value );
                    }
                }
            }

            if ( m_clock_chain_best_source_offset >= 0 ) {
                snap.diag_probe_clock_source_offset = m_clock_chain_best_source_offset;
                snap.diag_probe_clock_time_offset = m_clock_chain_best_time_offset;
                snap.diag_probe_clock_chain_hits = m_clock_chain_best_hits;
                snap.diag_probe_clock_chain_value = m_clock_chain_best_value;
            }
        }

        void probe_bindable_value( memory::c_process& process, uint64_t beatmap_bindable, osu::game_snapshot_t& snap ) {
            if ( !plausible_ptr( beatmap_bindable ) )
                return;

            int best_score = 0;
            int best_off = -1;
            int best_map_id = 0;
            uint64_t best_working = 0;
            std::string best_diff;

            for ( uint32_t off = 0x08; off <= 0x800; off += 0x08 ) {
                const uint64_t working = process.read<uint64_t>( beatmap_bindable + off );
                if ( !plausible_ptr( working ) )
                    continue;

                int score = 0;
                const uint64_t info = process.read<uint64_t>( working + m_off.working_map_info );
                const uint64_t set_info = process.read<uint64_t>( working + m_off.working_map_set_info );
                if ( plausible_ptr( info ) ) score += 2;
                if ( plausible_ptr( set_info ) ) score += 1;

                int map_id = 0;
                std::string hash;
                std::string diff;
                if ( plausible_ptr( info ) ) {
                    map_id = process.read<int32_t>( info + m_off.map_info_online_id );
                    if ( map_id > 0 && map_id < 100000000 ) score += 2;
                    if ( process.read_dotnet_string( info + m_off.map_info_hash, hash ) && hash.size( ) >= 8 ) score += 3;
                    if ( process.read_dotnet_string( info + m_off.map_info_difficulty, diff ) && !diff.empty( ) && diff.size( ) < 128 ) score += 3;
                }

                if ( score > best_score ) {
                    best_score = score;
                    best_off = static_cast<int>( off );
                    best_map_id = map_id;
                    best_working = working;
                    best_diff = diff;
                }
            }

            if ( best_off >= 0 ) {
                snap.diag_probe_bindable_offset = best_off;
                snap.diag_probe_bindable_score = best_score;
                snap.diag_probe_map_id = best_map_id;
                snap.diag_probe_working_beatmap = best_working;
                snap.diag_probe_difficulty = best_diff;

                if ( best_score >= m_last_bindable_probe_score ) {
                    const bool changed = m_last_bindable_probe_offset != best_off || m_last_bindable_probe_score != best_score;
                    m_last_bindable_probe_offset = best_off;
                    m_last_bindable_probe_score = best_score;
                    m_last_bindable_probe_map_id = best_map_id;
                    m_last_bindable_probe_working = best_working;
                    m_last_bindable_probe_difficulty = best_diff;
                    m_last_probe_seen_ms = GetTickCount64( );
                    if ( changed )
                        dbg::log( "BINDABLE candidate off=0x%X score=%d map=%d working=0x%llX diff=%s", m_last_bindable_probe_offset, m_last_bindable_probe_score, m_last_bindable_probe_map_id, static_cast<unsigned long long>( m_last_bindable_probe_working ), m_last_bindable_probe_difficulty.c_str( ) );
                }
            }
        }

        void probe_screen_stack_field( memory::c_process& process, uint64_t screen_stack, osu::game_snapshot_t& snap ) {
            if ( !plausible_ptr( screen_stack ) )
                return;

            const uint64_t base_api = process.read<uint64_t>( m_game_base + m_off.game_base_api );
            int best_score = 0;
            int best_off = -1;
            int best_count = 0;
            uint64_t best_screen = 0;
            bool best_api = false;
            bool best_ruleset = false;

            // ScreenStack inherits a large drawable hierarchy, so its private
            // Stack<IScreen> field may shift whenever framework base fields move.
            for ( uint32_t off = 0x08; off <= 0x1200; off += 0x08 ) {
                const uint64_t candidate_stack = process.read<uint64_t>( screen_stack + off );
                if ( !plausible_ptr( candidate_stack ) )
                    continue;

                const uint64_t items = process.read<uint64_t>( candidate_stack + 0x08 );
                const int count = process.read<int32_t>( candidate_stack + 0x10 );
                if ( !plausible_ptr( items ) || count <= 0 || count > 64 )
                    continue;

                const uint64_t current = process.read<uint64_t>( items + m_off.array_first_element + 8ull * static_cast<uint64_t>( count - 1 ) );
                if ( !plausible_ptr( current ) )
                    continue;

                int score = 2;
                const uint64_t submitting_api = process.read<uint64_t>( current + m_off.submitting_player_api );
                const uint64_t player_api = process.read<uint64_t>( current + m_off.player_api );
                const bool api_match = plausible_ptr( base_api ) && ( submitting_api == base_api || player_api == base_api );
                const uint64_t ruleset = process.read<uint64_t>( current + m_off.player_drawable_ruleset );
                const bool ruleset_ok = plausible_ptr( ruleset );
                if ( api_match ) score += 8;
                if ( ruleset_ok ) score += 3;

                // Prefer candidates near the previous field offset when scores tie.
                if ( score > best_score || ( score == best_score && best_off >= 0 &&
                     std::abs( static_cast<int>( off ) - static_cast<int>( m_off.screen_stack_stack ) ) <
                     std::abs( best_off - static_cast<int>( m_off.screen_stack_stack ) ) ) ) {
                    best_score = score;
                    best_off = static_cast<int>( off );
                    best_count = count;
                    best_screen = current;
                    best_api = api_match;
                    best_ruleset = ruleset_ok;
                }
            }

            if ( best_off >= 0 ) {
                snap.diag_probe_stack_offset = best_off;
                snap.diag_probe_stack_count = best_count;
                snap.diag_probe_current_screen = best_screen;
                snap.diag_probe_stack_api_match = best_api;
                snap.diag_probe_stack_ruleset = best_ruleset;

                if ( best_score >= m_last_stack_probe_score ) {
                    const bool changed = m_last_stack_probe_offset != best_off || m_last_stack_probe_score != best_score || m_last_stack_probe_count != best_count;
                    m_last_stack_probe_score = best_score;
                    m_last_stack_probe_offset = best_off;
                    m_last_stack_probe_count = best_count;
                    m_last_stack_probe_screen = best_screen;
                    m_last_stack_probe_api = best_api;
                    m_last_stack_probe_ruleset = best_ruleset;
                    m_last_probe_seen_ms = GetTickCount64( );
                    if ( changed )
                        dbg::log( "STACK candidate off=0x%X score=%d count=%d screen=0x%llX api=%d ruleset=%d", m_last_stack_probe_offset, m_last_stack_probe_score, m_last_stack_probe_count, static_cast<unsigned long long>( m_last_stack_probe_screen ), m_last_stack_probe_api ? 1 : 0, m_last_stack_probe_ruleset ? 1 : 0 );
                }
            }
        }

        void publish_latched_probes( osu::game_snapshot_t& snap ) {
            if ( snap.diag_probe_stack_offset < 0 && m_last_stack_probe_offset >= 0 ) {
                snap.diag_probe_stack_offset = m_last_stack_probe_offset;
                snap.diag_probe_stack_count = m_last_stack_probe_count;
                snap.diag_probe_current_screen = m_last_stack_probe_screen;
                snap.diag_probe_stack_api_match = m_last_stack_probe_api;
                snap.diag_probe_stack_ruleset = m_last_stack_probe_ruleset;
            }

            if ( snap.diag_probe_bindable_offset < 0 && m_last_bindable_probe_offset >= 0 ) {
                snap.diag_probe_bindable_offset = m_last_bindable_probe_offset;
                snap.diag_probe_bindable_score = m_last_bindable_probe_score;
                snap.diag_probe_map_id = m_last_bindable_probe_map_id;
                snap.diag_probe_working_beatmap = m_last_bindable_probe_working;
                snap.diag_probe_difficulty = m_last_bindable_probe_difficulty;
            }

            if ( snap.diag_probe_clock_offset < 0 && m_last_clock_probe_offset >= 0 ) {
                snap.diag_probe_clock_offset = m_last_clock_probe_offset;
                snap.diag_probe_clock_hits = m_last_clock_probe_hits;
                snap.diag_probe_clock_value = m_last_clock_probe_value;
            }

            // The deep clock probe already keeps its best candidate internally.
            if ( snap.diag_probe_clock_source_offset < 0 && m_clock_chain_best_source_offset >= 0 ) {
                snap.diag_probe_clock_source_offset = m_clock_chain_best_source_offset;
                snap.diag_probe_clock_time_offset = m_clock_chain_best_time_offset;
                snap.diag_probe_clock_chain_hits = m_clock_chain_best_hits;
                snap.diag_probe_clock_chain_value = m_clock_chain_best_value;
            }

            snap.diag_hold_clock_obj = m_hold_clock_obj;
            snap.diag_hold_clock_final = m_hold_clock_final;
            snap.diag_hold_clock_time = m_hold_clock_time;
            snap.diag_hold_beatmap_bind = m_hold_beatmap_bind;
            snap.diag_hold_beatmap_value = m_hold_beatmap_value;
            snap.diag_hold_stack_obj = m_hold_stack_obj;
            snap.diag_hold_stack_inner = m_hold_stack_inner;
            snap.diag_hold_stack_count = m_hold_stack_count;

            if ( m_last_probe_seen_ms != 0 ) {
                const ULONGLONG now = GetTickCount64( );
                const ULONGLONG age = now >= m_last_probe_seen_ms ? now - m_last_probe_seen_ms : 0;
                snap.diag_probe_capture_age_ms = age > 0x7fffffffULL ? 0x7fffffff : static_cast<int32_t>( age );
            }
        }

        void log_diag_snapshot( const osu::game_snapshot_t& snap ) {
            const ULONGLONG now = GetTickCount64( );
            if ( now - m_last_diag_log_ms < 250 )
                return;
            m_last_diag_log_ms = now;

            dbg::log(
                "LAZER status base=0x%llX state=%d time=%d clock=0x%llX final=0x%llX clock_ok=%d "
                "beat_bind=0x%llX working=0x%llX beat_ok=%d stack_obj=0x%llX stack=0x%llX count=%d stack_ok=%d "
                "screen=0x%llX base_api=0x%llX submit=0x%llX player=0x%llX ruleset=0x%llX apiS=%d apiP=%d "
                "probe_stack=0x%X/%d probe_bind=0x%X/%d/map%d probe_clock=0x%X/%d/v%d chain=0x%X+0x%X/%d/v%d",
                static_cast<unsigned long long>( snap.game_base ),
                static_cast<int>( snap.cur_state ),
                snap.cur_time,
                static_cast<unsigned long long>( snap.diag_beatmap_clock ),
                static_cast<unsigned long long>( snap.diag_final_source ),
                snap.diag_clock_ok ? 1 : 0,
                static_cast<unsigned long long>( snap.diag_beatmap_bindable ),
                static_cast<unsigned long long>( snap.diag_working_beatmap ),
                snap.diag_beatmap_ok ? 1 : 0,
                static_cast<unsigned long long>( snap.diag_screen_stack ),
                static_cast<unsigned long long>( snap.diag_stack ),
                snap.diag_stack_count,
                snap.diag_stack_ok ? 1 : 0,
                static_cast<unsigned long long>( snap.diag_current_screen ),
                static_cast<unsigned long long>( snap.diag_base_api ),
                static_cast<unsigned long long>( snap.diag_submitting_api ),
                static_cast<unsigned long long>( snap.diag_player_api ),
                static_cast<unsigned long long>( snap.diag_drawable_ruleset ),
                snap.diag_submitting_api_match ? 1 : 0,
                snap.diag_player_api_match ? 1 : 0,
                snap.diag_probe_stack_offset,
                snap.diag_probe_stack_count,
                snap.diag_probe_bindable_offset,
                snap.diag_probe_bindable_score,
                snap.diag_probe_map_id,
                snap.diag_probe_clock_offset,
                snap.diag_probe_clock_hits,
                snap.diag_probe_clock_value,
                snap.diag_probe_clock_source_offset,
                snap.diag_probe_clock_time_offset,
                snap.diag_probe_clock_chain_hits,
                snap.diag_probe_clock_chain_value );

            dbg::log(
                "AUTO map=%d hashlen=%u verlen=%u dyn_stack_field=0x%X nested=0x%X items=0x%X count=0x%X elem=0x%X api=0x%X ruleset=0x%X beatmap=0x%X hitobjects=0x%X dyn_clock=0x%X+0x%X+0x%X hits=%d player_clock=0x%X+0x%X+0x%X phits=%d",
                snap.map_id,
                static_cast<unsigned>( snap.beatmap_hash.size( ) ),
                static_cast<unsigned>( snap.beatmap_version.size( ) ),
                m_dyn_screen_stack_field,
                m_dyn_screen_stack_nested_field,
                m_dyn_stack_items_offset,
                m_dyn_stack_count_offset,
                m_dyn_array_element_offset,
                m_dyn_screen_api_offset,
                m_dyn_ruleset_offset,
                m_dyn_drawable_beatmap_offset,
                m_dyn_hitobjects_offset,
                m_dyn_game_clock_offset,
                m_dyn_clock_source_offset,
                m_dyn_clock_time_offset,
                m_dyn_clock_hits,
                m_dyn_player_clock_field,
                m_dyn_player_clock_nested,
                m_dyn_player_clock_time,
                m_dyn_player_clock_hits );
        }

        void update( memory::c_process& process, osu::game_snapshot_t& snap ) override {
            snap.client = osu::client_kind_t::lazer;
            snap.game_base = m_game_base;
            snap.offset_version = m_off.osu_version;
            snap.player_screen = 0;
            snap.drawable_ruleset = 0;
            snap.cur_mod_state = 0;
            snap.speed_mult = 1.f;

            // v5: do not trust the legacy Game-level offsets to identify the managed
            // Game object. 2026.804.2 gives us a much stronger signature through the
            // exact Player layout. Recover Player first, then take Game/API directly
            // from Player +0x400/+0x3F0. This also survives the old base scanner finding
            // unrelated managed/native addresses.
            refresh_exact_player_structurally( process );

            if ( !ensure_game_base( process ) ) {
                snap.game_base = m_game_base;
                publish_latched_probes( snap );
                log_diag_snapshot( snap );
                return;
            }

            // ensure_game_base() may have refreshed a moved managed object.
            snap.game_base = m_game_base;
            snap.cur_mod_state = read_lazer_mods( process );

            const auto beatmap_clock = process.read<uint64_t>( m_game_base + m_off.game_base_beatmap_clock );
            snap.diag_beatmap_clock = beatmap_clock;
            if ( beatmap_clock ) {
                m_hold_clock_obj = true;
                m_last_probe_seen_ms = GetTickCount64( );
                probe_clock_chain( process, beatmap_clock, snap );
                const auto final_source = process.read<uint64_t>( beatmap_clock + m_off.framed_clock_final_source );
                snap.diag_final_source = final_source;
                if ( final_source ) {
                    m_hold_clock_final = true;
                    m_last_probe_seen_ms = GetTickCount64( );
                    const auto raw_time = process.read<double>( final_source + m_off.framed_clock_current_time );
                    if ( std::isfinite( raw_time ) && raw_time > -60000.0 && raw_time < 86400000.0 ) {
                        snap.diag_clock_ok = true;
                        m_hold_clock_time = true;
                        m_last_probe_seen_ms = GetTickCount64( );
                        snap.diag_raw_time = static_cast<int32_t>( raw_time );
                        snap.cur_time = snap.diag_raw_time;
                    }

                    probe_clock_field( process, final_source, snap );

                    const auto f1 = process.read<uint64_t>( final_source + 8 );
                    const auto f2 = f1 ? process.read<uint64_t>( f1 + 8 ) : 0;
                    const auto f3 = f2 ? process.read<uint64_t>( f2 + 8 ) : 0;
                    if ( f3 ) {
                        double rate = process.read<double>( f3 + 144 );
                        if ( rate > 0.01 && rate < 5.0 ) {
                            snap.speed_mult = static_cast<float>( rate );
                        }
                    }
                }
            }

            if ( !snap.diag_clock_ok ) {
                double dynamic_time = 0.0;
                if ( try_dynamic_clock( process, dynamic_time ) ) {
                    snap.cur_time = static_cast<int32_t>( dynamic_time );
                    snap.diag_raw_time = snap.cur_time;
                    snap.diag_clock_ok = true;
                    if ( m_dyn_game_clock_offset >= 0 ) {
                        const uint64_t dyn_clock = process.read<uint64_t>( m_game_base + static_cast<uint32_t>( m_dyn_game_clock_offset ) );
                        snap.diag_beatmap_clock = dyn_clock;
                        if ( plausible_ptr( dyn_clock ) && m_dyn_clock_source_offset >= 0 )
                            snap.diag_final_source = process.read<uint64_t>( dyn_clock + static_cast<uint32_t>( m_dyn_clock_source_offset ) );
                    }
                }
            }

            if ( snap.speed_mult == 1.f ) {
                if ( ( snap.cur_mod_state & 64 ) != 0 || ( snap.cur_mod_state & 512 ) != 0 ) {
                    snap.speed_mult = 1.5f;
                } else if ( ( snap.cur_mod_state & 256 ) != 0 ) {
                    snap.speed_mult = 0.75f;
                }
            }

            const auto beatmap_bindable = process.read<uint64_t>( m_game_base + m_off.game_base_beatmap );
            snap.diag_beatmap_bindable = beatmap_bindable;
            if ( beatmap_bindable ) {
                m_hold_beatmap_bind = true;
                m_last_probe_seen_ms = GetTickCount64( );
                const auto working_beatmap = process.read<uint64_t>( beatmap_bindable + m_off.bindable_value );
                snap.diag_working_beatmap = working_beatmap;
                snap.diag_beatmap_ok = working_beatmap != 0;
                if ( working_beatmap ) {
                    m_hold_beatmap_value = true;
                    m_last_probe_seen_ms = GetTickCount64( );
                }
                if ( working_beatmap ) {
                    const auto beatmap_info = process.read<uint64_t>( working_beatmap + m_off.working_map_info );
                    const auto set_info = process.read<uint64_t>( working_beatmap + m_off.working_map_set_info );
                    if ( beatmap_info ) {
                        snap.map_id = process.read<int32_t>( beatmap_info + m_off.map_info_online_id );
                        process.read_dotnet_string( beatmap_info + m_off.map_info_hash, snap.beatmap_hash );
                        process.read_dotnet_string( beatmap_info + m_off.map_info_difficulty, snap.beatmap_version );
                    }
                    if ( set_info )
                        snap.set_id = process.read<int32_t>( set_info + m_off.set_info_online_id );
                }
            }

            if ( beatmap_bindable && !snap.diag_beatmap_ok )
                probe_bindable_value( process, beatmap_bindable, snap );

            const bool have_map_identity = snap.map_id > 0 || !snap.beatmap_hash.empty( ) ||
                ( snap.set_id > 0 && !snap.beatmap_version.empty( ) );
            if ( beatmap_bindable && !have_map_identity )
                try_resolve_beatmap_metadata( process, beatmap_bindable, snap );

            uint64_t base_api = process.read<uint64_t>( m_game_base + m_off.game_base_api );
            if ( validate_structural_player( process, m_exact_player_hint ) &&
                 process.read<uint64_t>( m_exact_player_hint + m_off.player_game ) == m_game_base ) {
                base_api = process.read<uint64_t>( m_exact_player_hint + m_off.player_api );
            }
            snap.diag_base_api = base_api;

            const auto screen_stack = process.read<uint64_t>( m_game_base + m_off.game_screen_stack );
            snap.diag_screen_stack = screen_stack;
            if ( screen_stack ) {
                m_hold_stack_obj = true;
                m_last_probe_seen_ms = GetTickCount64( );
            }

            uint64_t stack_holder = 0;
            uint64_t current_screen = 0;
            sequence_layout_t seq{};

            // Once Player has been found, keep the hot path cheap and independent of
            // the stale Game.ScreenStack field. Only probe the stack again if the cached
            // managed object no longer validates (for example after a compacting GC).
            bool auto_stack_ok = false;
            if ( validate_exact_player( process, m_exact_player_hint, base_api ) ) {
                current_screen = m_exact_player_hint;
            } else {
                m_exact_player_hint = 0;
                auto_stack_ok = resolve_screen_stack_auto( process, screen_stack, base_api, stack_holder, seq, current_screen );
            }

            if ( auto_stack_ok ) {
                snap.diag_stack = stack_holder;
                snap.diag_stack_count = seq.count;
                snap.diag_stack_ok = true;
                m_hold_stack_inner = true;
                m_hold_stack_count = seq.count;
                m_exact_player_hint = current_screen;
            } else {
                // game_screen_stack is one of the offsets that moved in recent lazer
                // builds. Do not make the whole reader depend on it. If no cached Player
                // survived, discover Player directly in the same managed heap region by
                // its exact 2026.804.2 field pair:
                //   Player +0x3F0 = API, Player +0x400 = this Game.
                if ( !current_screen ) {
                    try_scan_exact_player_near_game( process, base_api, current_screen );
                    if ( current_screen )
                        m_exact_player_hint = current_screen;
                }

                // Preserve the legacy values in diagnostics to make comparison with
                // the old reader easy, but do not trust them as a valid Stack<T>.
                const auto legacy_stack = screen_stack ? process.read<uint64_t>( screen_stack + m_off.screen_stack_stack ) : 0;
                const auto legacy_count = legacy_stack ? process.read<int32_t>( legacy_stack + 0x10 ) : 0;
                snap.diag_stack = legacy_stack;
                snap.diag_stack_count = legacy_count;
                snap.diag_stack_ok = false;

                if ( !current_screen ) {
                    probe_screen_stack_field( process, screen_stack, snap );
                    publish_latched_probes( snap );
                    log_diag_snapshot( snap );
                    return;
                }
            }

            snap.player_screen = current_screen;
            snap.diag_current_screen = current_screen;
            snap.diag_current_screen_ok = plausible_ptr( current_screen );
            if ( !snap.diag_current_screen_ok ) {
                publish_latched_probes( snap );
                log_diag_snapshot( snap );
                return;
            }

            // resolve_screen_stack_auto() now returns the real Player, never the
            // PlayerLoader. Validate the exact 2026.804.2 Player layout once more here
            // before following gameplay-only fields.
            const auto submitting_api = process.read<uint64_t>( current_screen + m_off.submitting_player_api );
            const auto player_api = process.read<uint64_t>( current_screen + m_off.player_api );
            snap.diag_submitting_api = submitting_api;
            snap.diag_player_api = player_api;
            snap.diag_submitting_api_match = api_links_game( process, submitting_api, base_api );
            snap.diag_player_api_match = api_links_game( process, player_api, base_api );

            const bool exact_player_ok = validate_exact_player( process, current_screen, base_api );
            if ( !exact_player_ok ) {
                publish_latched_probes( snap );
                log_diag_snapshot( snap );
                return;
            }

            const uint64_t gameplay_state = process.read<uint64_t>(
                current_screen + m_off.player_gameplay_state );
            const uint64_t gameplay_beatmap = plausible_ptr( gameplay_state )
                ? process.read<uint64_t>( gameplay_state + m_off.gameplay_state_beatmap ) : 0;
            const uint64_t gameplay_clock_container = process.read<uint64_t>(
                current_screen + m_off.player_gameplay_clock_container );

            snap.gameplay_state = plausible_ptr( gameplay_state ) ? gameplay_state : 0;
            snap.gameplay_beatmap = plausible_ptr( gameplay_beatmap ) ? gameplay_beatmap : 0;
            snap.gameplay_clock_container = plausible_ptr( gameplay_clock_container ) ? gameplay_clock_container : 0;

            const int32_t gameplay_mods = read_lazer_mods_from_gameplay_state( process, snap.gameplay_state );
            if ( gameplay_mods >= 0 ) {
                snap.cur_mod_state = gameplay_mods;
                if ( ( gameplay_mods & 64 ) != 0 || ( gameplay_mods & 512 ) != 0 )
                    snap.speed_mult = 1.5f;
                else if ( ( gameplay_mods & 256 ) != 0 )
                    snap.speed_mult = 0.75f;
                else
                    snap.speed_mult = 1.f;
            }

            uint64_t gameplay_items = 0;
            int32_t gameplay_count = 0;
            bool gameplay_beatmap_ok = false;
            if ( snap.gameplay_beatmap ) {
                const uint64_t list = process.read<uint64_t>(
                    snap.gameplay_beatmap + m_off.beatmap_hit_objects );
                gameplay_beatmap_ok = strict_list_layout( process, list, gameplay_items, gameplay_count );

                // GameplayState.Beatmap also carries the exact BeatmapInfo object at
                // +0x10. Use it to recover metadata even when the old Game-level
                // selected-beatmap Bindable moved.
                const uint64_t info = process.read<uint64_t>( snap.gameplay_beatmap + m_off.beatmap_info );
                if ( plausible_ptr( info ) ) {
                    const int32_t exact_map_id = process.read<int32_t>( info + m_off.map_info_online_id );
                    if ( exact_map_id > 0 )
                        snap.map_id = exact_map_id;
                    std::string exact_hash;
                    if ( process.read_dotnet_string( info + m_off.map_info_hash, exact_hash ) && !exact_hash.empty( ) )
                        snap.beatmap_hash = std::move( exact_hash );
                    std::string exact_version;
                    if ( process.read_dotnet_string( info + m_off.map_info_difficulty, exact_version ) && !exact_version.empty( ) )
                        snap.beatmap_version = std::move( exact_version );

                    const uint64_t set_info = process.read<uint64_t>( info + m_off.map_info_set_info );
                    if ( plausible_ptr( set_info ) ) {
                        const int32_t exact_set_id = process.read<int32_t>( set_info + m_off.set_info_online_id );
                        if ( exact_set_id > 0 )
                            snap.set_id = exact_set_id;
                    }
                }
            }

            // DrawableRuleset is still useful for other readers, but it is no longer
            // required to load hit objects. GameplayState.Beatmap is a shorter and much
            // less fragile chain.
            uint64_t drawable_ruleset = process.read<uint64_t>(
                current_screen + m_off.player_drawable_ruleset );
            bool ruleset_valid = false;
            if ( plausible_ptr( drawable_ruleset ) ) {
                const uint64_t ruleset_beatmap = process.read<uint64_t>(
                    drawable_ruleset + m_off.drawable_osu_beatmap );
                if ( plausible_ptr( ruleset_beatmap ) ) {
                    if ( gameplay_beatmap_ok && ruleset_beatmap == snap.gameplay_beatmap ) {
                        ruleset_valid = true;
                    } else {
                        const uint64_t list = process.read<uint64_t>(
                            ruleset_beatmap + m_off.beatmap_hit_objects );
                        uint64_t items = 0;
                        int32_t count = 0;
                        ruleset_valid = strict_list_layout( process, list, items, count );
                        if ( ruleset_valid && !gameplay_beatmap_ok ) {
                            snap.gameplay_beatmap = ruleset_beatmap;
                            gameplay_beatmap_ok = true;
                            gameplay_count = count;
                        }
                    }
                }
            }
            if ( !ruleset_valid )
                drawable_ruleset = 0;

            snap.diag_drawable_ruleset = drawable_ruleset;
            snap.diag_ruleset_ok = ruleset_valid;
            snap.drawable_ruleset = drawable_ruleset;

            // Prefer the exact Player -> GameplayClockContainer -> GameplayClock path.
            // It excludes MasterGameplayClockContainer.elapsedGameplayClockTime, which
            // is only a validation accumulator and not the beatmap timeline.
            double player_time = 0.0;
            if ( try_exact_gameplay_clock( process, current_screen, snap.speed_mult, player_time ) ) {
                snap.cur_time = static_cast<int32_t>( player_time );
                snap.diag_raw_time = snap.cur_time;
                snap.diag_clock_ok = true;
            } else if ( m_exact_clock_scan_count > 8 &&
                        try_player_clock( process, current_screen,
                            static_cast<int32_t>( m_off.player_drawable_ruleset ), snap.speed_mult, player_time ) ) {
                // Broad graph scan is retained only as a delayed fallback.
                snap.cur_time = static_cast<int32_t>( player_time );
                snap.diag_raw_time = snap.cur_time;
                snap.diag_clock_ok = true;
            }

            const ULONGLONG exact_now = GetTickCount64( );
            if ( m_last_exact_layout_log_ms == 0 || exact_now - m_last_exact_layout_log_ms >= 1000 ) {
                m_last_exact_layout_log_ms = exact_now;
                dbg::log( "EXACT player=0x%llX state=0x%llX beatmap=0x%llX objects=%d map=%d set=%d ruleset=0x%llX clock_container=0x%llX time=%d clock_ok=%d",
                    static_cast<unsigned long long>( current_screen ),
                    static_cast<unsigned long long>( snap.gameplay_state ),
                    static_cast<unsigned long long>( snap.gameplay_beatmap ),
                    gameplay_count, snap.map_id, snap.set_id,
                    static_cast<unsigned long long>( snap.drawable_ruleset ),
                    static_cast<unsigned long long>( snap.gameplay_clock_container ),
                    snap.cur_time, snap.diag_clock_ok ? 1 : 0 );
            }

            const bool api_match = snap.diag_player_api_match;
            const bool gameplay_evidence = api_match && gameplay_beatmap_ok && snap.gameplay_state != 0;
            if ( gameplay_evidence )
                snap.cur_state = osu::game_state_t::play;
            else
                snap.cur_state = osu::game_state_t::main_menu;

            publish_latched_probes( snap );
            log_diag_snapshot( snap );
        }

        osu::client_kind_t kind( ) const override { return osu::client_kind_t::lazer; }

        [[nodiscard]] const offsets::lazer::table_t& offsets( ) const { return m_off; }

    private:
        offsets::lazer::table_t& m_off;
        uint64_t m_game_base = 0;
        uint64_t m_game_root_slot = 0;
        ULONGLONG m_last_reacquire_ms = 0;
        uint32_t m_reacquire_count = 0;
        uint64_t m_exact_player_hint = 0;
        ULONGLONG m_last_exact_player_scan_ms = 0;
        ULONGLONG m_last_structural_player_scan_ms = 0;
        resolved_mod_tokens_t m_tokens;
        std::array<double, 128> m_clock_probe_prev{};
        std::array<int32_t, 128> m_clock_probe_hits{};

        static constexpr size_t clock_chain_source_slots = 256;
        static constexpr size_t clock_chain_time_slots = 64;
        std::array<double, clock_chain_source_slots * clock_chain_time_slots> m_clock_chain_prev{};
        std::array<int32_t, clock_chain_source_slots * clock_chain_time_slots> m_clock_chain_hits{};
        ULONGLONG m_last_clock_chain_probe_ms = 0;
        ULONGLONG m_last_diag_log_ms = 0;
        int32_t m_clock_chain_best_source_offset = -1;
        int32_t m_clock_chain_best_time_offset = -1;
        int32_t m_clock_chain_best_hits = 0;
        int32_t m_clock_chain_best_value = 0;

        // Hold the strongest transient probe result so it remains visible after
        // gameplay transitions back to a state where the live chain is null.
        int32_t m_last_stack_probe_score = 0;
        int32_t m_last_stack_probe_offset = -1;
        int32_t m_last_stack_probe_count = 0;
        uint64_t m_last_stack_probe_screen = 0;
        bool m_last_stack_probe_api = false;
        bool m_last_stack_probe_ruleset = false;

        int32_t m_last_bindable_probe_offset = -1;
        int32_t m_last_bindable_probe_score = 0;
        int32_t m_last_bindable_probe_map_id = 0;
        uint64_t m_last_bindable_probe_working = 0;
        std::string m_last_bindable_probe_difficulty;

        int32_t m_last_clock_probe_offset = -1;
        int32_t m_last_clock_probe_hits = 0;
        int32_t m_last_clock_probe_value = 0;
        ULONGLONG m_last_probe_seen_ms = 0;

        bool m_hold_clock_obj = false;
        bool m_hold_clock_final = false;
        bool m_hold_clock_time = false;
        bool m_hold_beatmap_bind = false;
        bool m_hold_beatmap_value = false;
        bool m_hold_stack_obj = false;
        bool m_hold_stack_inner = false;
        int32_t m_hold_stack_count = 0;

        struct sequence_layout_t {
            bool ok = false;
            uint64_t items = 0;
            int32_t count = 0;
            uint32_t items_offset = 0;
            uint32_t count_offset = 0;
            uint32_t element_offset = 0;
            int score = 0;
        };

        struct hit_layout_t {
            bool ok = false;
            uint32_t start_bindable_offset = 0;
            uint32_t bindable_number_offset = 0;
            uint32_t position_offset = 0;
            int score = 0;
            int valid_times = 0;
        };

        int32_t m_dyn_screen_stack_field = -1;
        int32_t m_dyn_screen_stack_nested_field = -1;
        int32_t m_dyn_stack_items_offset = -1;
        int32_t m_dyn_stack_count_offset = -1;
        int32_t m_dyn_array_element_offset = -1;
        int32_t m_dyn_screen_api_offset = -1;
        int32_t m_dyn_ruleset_offset = -1;
        int32_t m_dyn_drawable_beatmap_offset = -1;
        int32_t m_dyn_hitobjects_offset = -1;
        ULONGLONG m_last_layout_log_ms = 0;
        ULONGLONG m_last_exact_layout_log_ms = 0;
        ULONGLONG m_last_screen_layout_probe_ms = 0;
        ULONGLONG m_last_metadata_probe_ms = 0;
        ULONGLONG m_last_ruleset_probe_ms = 0;

        struct clock_track_t {
            double prev = 0.0;
            bool have_prev = false;
            int32_t hits = 0;
        };
        std::unordered_map<uint64_t, clock_track_t> m_root_clock_tracks;
        ULONGLONG m_last_root_clock_scan_ms = 0;
        int32_t m_dyn_game_clock_offset = -1;
        int32_t m_dyn_clock_source_offset = -1;
        int32_t m_dyn_clock_time_offset = -1;
        int32_t m_dyn_clock_hits = 0;

        std::unordered_map<uint64_t, clock_track_t> m_player_clock_tracks;
        ULONGLONG m_last_player_clock_scan_ms = 0;
        ULONGLONG m_prev_player_clock_scan_ms = 0;
        int32_t m_dyn_player_clock_field = -1;
        int32_t m_dyn_player_clock_nested = -1; // -1 means the double is directly in the first child.
        int32_t m_dyn_player_clock_time = -1;
        int32_t m_dyn_player_clock_hits = 0;
        int32_t m_player_clock_scan_count = 0;

        std::unordered_map<uint64_t, clock_track_t> m_exact_clock_tracks;
        ULONGLONG m_last_exact_clock_scan_ms = 0;
        ULONGLONG m_prev_exact_clock_scan_ms = 0;
        int32_t m_exact_clock_nested = -2; // -1 = direct FramedBeatmapClock, >=0 = one pointer hop.
        int32_t m_exact_clock_time = -1;
        int32_t m_exact_clock_hits = 0;
        int32_t m_exact_clock_scan_count = 0;

        sequence_layout_t resolve_sequence_layout( memory::c_process& process, uint64_t obj, int32_t max_count = 128 ) {
            sequence_layout_t best{};
            if ( !plausible_ptr( obj ) )
                return best;

            static constexpr uint32_t element_offsets[] = { 0x10, 0x18, 0x20, 0x28 };

            for ( uint32_t items_off = 0x08; items_off <= 0x50; items_off += 0x08 ) {
                const uint64_t items = process.read<uint64_t>( obj + items_off );
                if ( !plausible_ptr( items ) )
                    continue;

                for ( uint32_t count_off = 0x0C; count_off <= 0x58; count_off += 0x04 ) {
                    if ( count_off >= items_off && count_off < items_off + 8 )
                        continue;

                    const int32_t count = process.read<int32_t>( obj + count_off );
                    if ( count <= 0 || count > max_count )
                        continue;

                    for ( uint32_t elem_off : element_offsets ) {
                        const uint64_t first = process.read<uint64_t>( items + elem_off );
                        const uint64_t last = process.read<uint64_t>( items + elem_off + 8ull * static_cast<uint64_t>( count - 1 ) );
                        if ( !plausible_ptr( first ) || !plausible_ptr( last ) )
                            continue;

                        int score = 6;
                        if ( count <= 32 ) score += 2;
                        if ( items_off == 0x08 || items_off == 0x10 ) score += 1;
                        if ( count_off == items_off + 8 ) score += 2;
                        if ( elem_off == 0x10 ) score += 1;

                        if ( score > best.score ) {
                            best.ok = true;
                            best.items = items;
                            best.count = count;
                            best.items_offset = items_off;
                            best.count_offset = count_off;
                            best.element_offset = elem_off;
                            best.score = score;
                        }
                    }
                }
            }

            return best;
        }

        int32_t find_pointer_field( memory::c_process& process, uint64_t obj, uint64_t target, uint32_t max_offset = 0x900 ) {
            if ( !plausible_ptr( obj ) || !plausible_ptr( target ) )
                return -1;

            int32_t best = -1;
            int32_t best_distance = 0x7fffffff;
            for ( uint32_t off = 0x08; off <= max_offset; off += 0x08 ) {
                if ( process.read<uint64_t>( obj + off ) != target )
                    continue;

                const int32_t d1 = std::abs( static_cast<int32_t>( off ) - static_cast<int32_t>( m_off.player_api ) );
                const int32_t d2 = std::abs( static_cast<int32_t>( off ) - static_cast<int32_t>( m_off.submitting_player_api ) );
                const int32_t d = (std::min)( d1, d2 );
                if ( d < best_distance ) {
                    best = static_cast<int32_t>( off );
                    best_distance = d;
                }
            }
            return best;
        }

        bool is_committed_private_ptr( memory::c_process& process, uint64_t p ) {
            if ( !plausible_ptr( p ) )
                return false;
            MEMORY_BASIC_INFORMATION mbi{};
            if ( !VirtualQueryEx( process.handle( ), reinterpret_cast<LPCVOID>( p ), &mbi, sizeof( mbi ) ) )
                return false;
            if ( mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE )
                return false;
            const DWORD protect = mbi.Protect & 0xff;
            return protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                   protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
        }

        bool validate_structural_player( memory::c_process& process, uint64_t player ) {
            if ( !is_committed_private_ptr( process, player ) )
                return false;

            const uint64_t api = process.read<uint64_t>( player + m_off.player_api );
            const uint64_t game = process.read<uint64_t>( player + m_off.player_game );
            const uint64_t state = process.read<uint64_t>( player + m_off.player_gameplay_state );
            const uint64_t ruleset = process.read<uint64_t>( player + m_off.player_drawable_ruleset );
            const uint64_t clock_container = process.read<uint64_t>( player + m_off.player_gameplay_clock_container );
            if ( !is_committed_private_ptr( process, api ) ||
                 !is_committed_private_ptr( process, game ) ||
                 !is_committed_private_ptr( process, state ) ||
                 !is_committed_private_ptr( process, ruleset ) ||
                 !is_committed_private_ptr( process, clock_container ) )
                return false;

            const uint64_t beatmap = process.read<uint64_t>( state + m_off.gameplay_state_beatmap );
            if ( !is_committed_private_ptr( process, beatmap ) )
                return false;

            const uint64_t list = process.read<uint64_t>( beatmap + m_off.beatmap_hit_objects );
            uint64_t items = 0;
            int32_t count = 0;
            if ( !strict_list_layout( process, list, items, count ) )
                return false;

            // Both exact chains must converge on the very same playable beatmap.
            const uint64_t ruleset_beatmap = process.read<uint64_t>( ruleset + m_off.drawable_osu_beatmap );
            if ( ruleset_beatmap != beatmap )
                return false;

            const uint64_t framed_clock = process.read<uint64_t>(
                clock_container + m_off.gameplay_clock_gameplay_clock );
            if ( !is_committed_private_ptr( process, framed_clock ) )
                return false;

            return count > 0;
        }

        bool scan_range_for_structural_player( memory::c_process& process, uint64_t start, size_t len,
                                               uint64_t& out_player ) {
            out_player = 0;
            constexpr size_t need = 0x480;
            if ( len <= need )
                return false;

            constexpr size_t chunk_size = 4 * 1024 * 1024;
            constexpr size_t overlap = 0x500;
            std::vector<uint8_t> buffer;

            for ( size_t offset = 0; offset < len; offset += chunk_size ) {
                const size_t primary = (std::min)( chunk_size, len - offset );
                const size_t to_read = (std::min)( len - offset, primary + overlap );
                if ( to_read <= need )
                    break;

                buffer.resize( to_read );
                if ( !process.read_buffer( start + offset, buffer.data( ), to_read ) )
                    continue;

                const size_t limit = (std::min)( primary, to_read - need );
                for ( size_t i = 0; i < limit; i += 8 ) {
                    uint64_t api = 0, game = 0, state = 0, ruleset = 0, clock = 0;
                    std::memcpy( &api, buffer.data( ) + i + m_off.player_api, 8 );
                    std::memcpy( &game, buffer.data( ) + i + m_off.player_game, 8 );
                    std::memcpy( &state, buffer.data( ) + i + m_off.player_gameplay_state, 8 );
                    std::memcpy( &ruleset, buffer.data( ) + i + m_off.player_drawable_ruleset, 8 );
                    std::memcpy( &clock, buffer.data( ) + i + m_off.player_gameplay_clock_container, 8 );
                    if ( !plausible_ptr( api ) || !plausible_ptr( game ) || !plausible_ptr( state ) ||
                         !plausible_ptr( ruleset ) || !plausible_ptr( clock ) )
                        continue;

                    const uint64_t candidate = start + offset + i;
                    if ( validate_structural_player( process, candidate ) ) {
                        out_player = candidate;
                        return true;
                    }
                }
            }
            return false;
        }

        bool try_scan_structural_player_near_hint( memory::c_process& process, uint64_t hint,
                                                    uint64_t& out_player ) {
            out_player = 0;
            if ( !plausible_ptr( hint ) )
                return false;

            // GC objects involved in one gameplay graph may live in neighbouring heap
            // segments rather than the exact same VirtualQueryEx region. Walk a bounded
            // window around the last managed hint instead of scanning the whole process.
            constexpr uint64_t radius = 256ull * 1024ull * 1024ull;
            const uint64_t window_start = hint > radius ? hint - radius : 0x10000ull;
            const uint64_t window_end = ( hint < 0x7FFFFFFFFFFFULL - radius )
                ? hint + radius : 0x7FFFFFFFFFFFULL;

            MEMORY_BASIC_INFORMATION mbi{};
            uint64_t addr = window_start;
            while ( addr < window_end &&
                    VirtualQueryEx( process.handle( ), reinterpret_cast<LPCVOID>( addr ), &mbi, sizeof( mbi ) ) ) {
                const uint64_t region_start = reinterpret_cast<uint64_t>( mbi.BaseAddress );
                const uint64_t region_end = region_start + static_cast<uint64_t>( mbi.RegionSize );
                const uint64_t scan_start = (std::max)( region_start, window_start );
                const uint64_t scan_end = (std::min)( region_end, window_end );

                const DWORD protect = mbi.Protect & 0xff;
                const bool writable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                    protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
                if ( mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && writable &&
                     scan_end > scan_start && scan_end - scan_start >= 0x10000 ) {
                    if ( scan_range_for_structural_player( process, scan_start,
                            static_cast<size_t>( scan_end - scan_start ), out_player ) )
                        return true;
                }

                if ( region_end <= addr )
                    break;
                addr = region_end;
            }
            return false;
        }

        bool refresh_exact_player_structurally( memory::c_process& process ) {
            if ( validate_structural_player( process, m_exact_player_hint ) ) {
                const uint64_t game = process.read<uint64_t>( m_exact_player_hint + m_off.player_game );
                if ( plausible_ptr( game ) )
                    m_game_base = game;
                return true;
            }

            const ULONGLONG now = GetTickCount64( );
            if ( m_last_structural_player_scan_ms != 0 && now - m_last_structural_player_scan_ms < 750 )
                return false;
            m_last_structural_player_scan_ms = now;

            uint64_t player = 0;
            if ( !try_scan_structural_player_near_hint( process, m_game_root_slot, player ) )
                try_scan_structural_player_near_hint( process, m_game_base, player );
            if ( !player )
                return false;

            m_exact_player_hint = player;
            m_game_base = process.read<uint64_t>( player + m_off.player_game );
            dbg::log( "BASE exact-player structural player=0x%llX game=0x%llX api=0x%llX state=0x%llX",
                static_cast<unsigned long long>( player ),
                static_cast<unsigned long long>( m_game_base ),
                static_cast<unsigned long long>( process.read<uint64_t>( player + m_off.player_api ) ),
                static_cast<unsigned long long>( process.read<uint64_t>( player + m_off.player_gameplay_state ) ) );
            return plausible_ptr( m_game_base );
        }

        bool api_links_game( memory::c_process& process, uint64_t api, uint64_t base_api ) {
            if ( !plausible_ptr( api ) )
                return false;
            if ( api == base_api )
                return true;

            if ( validate_structural_player( process, m_exact_player_hint ) ) {
                const uint64_t exact_api = process.read<uint64_t>( m_exact_player_hint + m_off.player_api );
                const uint64_t exact_game = process.read<uint64_t>( m_exact_player_hint + m_off.player_game );
                if ( api == exact_api && exact_game == m_game_base )
                    return true;
            }

            return process.read<uint64_t>( api + m_off.api_access_game ) == m_game_base;
        }

        int32_t find_game_api_link_field( memory::c_process& process, uint64_t obj, uint64_t base_api,
                                          uint32_t max_offset = 0x1800 ) {
            if ( !plausible_ptr( obj ) || !plausible_ptr( base_api ) )
                return -1;

            // Prefer the exact Game API pointer.
            for ( uint32_t off = 0x08; off <= max_offset; off += 0x08 ) {
                if ( process.read<uint64_t>( obj + off ) == base_api )
                    return static_cast<int32_t>( off );
            }

            // Some current lazer screens keep another API/provider object instead of
            // the exact pointer stored by Game. It is still the right API if its
            // api_access_game field resolves back to this exact Game instance.
            for ( uint32_t off = 0x08; off <= max_offset; off += 0x08 ) {
                const uint64_t p = process.read<uint64_t>( obj + off );
                if ( p == obj || p == m_game_base || !plausible_ptr( p ) )
                    continue;
                if ( process.read<uint64_t>( p + m_off.api_access_game ) == m_game_base )
                    return static_cast<int32_t>( off );
            }

            return -1;
        }

        bool validate_exact_player( memory::c_process& process, uint64_t player, uint64_t base_api ) {
            if ( !validate_structural_player( process, player ) )
                return false;

            const uint64_t game = process.read<uint64_t>( player + m_off.player_game );
            if ( game != m_game_base )
                return false;

            const uint64_t api = process.read<uint64_t>( player + m_off.player_api );
            if ( plausible_ptr( base_api ) && api != base_api && !api_links_game( process, api, base_api ) )
                return false;

            return true;
        }

        bool scan_range_for_exact_player( memory::c_process& process, uint64_t start, size_t len,
                                          uint64_t base_api, uint64_t& out_player ) {
            out_player = 0;
            if ( len <= static_cast<size_t>( m_off.player_game + 8 ) )
                return false;

            constexpr size_t chunk_size = 4 * 1024 * 1024;
            constexpr size_t overlap = 0x500;
            std::vector<uint8_t> buffer;

            for ( size_t offset = 0; offset < len; offset += chunk_size ) {
                const size_t primary = (std::min)( chunk_size, len - offset );
                const size_t to_read = (std::min)( len - offset, primary + overlap );
                if ( to_read <= static_cast<size_t>( m_off.player_game + 8 ) )
                    break;

                buffer.resize( to_read );
                if ( !process.read_buffer( start + offset, buffer.data( ), to_read ) )
                    continue;

                const size_t limit = (std::min)( primary, to_read - static_cast<size_t>( m_off.player_game + 8 ) );
                for ( size_t i = 0; i < limit; i += 8 ) {
                    uint64_t api = 0;
                    uint64_t game = 0;
                    std::memcpy( &api, buffer.data( ) + i + m_off.player_api, sizeof( api ) );
                    std::memcpy( &game, buffer.data( ) + i + m_off.player_game, sizeof( game ) );
                    if ( game != m_game_base || !plausible_ptr( api ) )
                        continue;

                    const uint64_t candidate = start + offset + i;
                    if ( validate_exact_player( process, candidate, base_api ) ) {
                        out_player = candidate;
                        return true;
                    }
                }
            }
            return false;
        }

        bool try_scan_exact_player_near_game( memory::c_process& process, uint64_t base_api,
                                               uint64_t& out_player ) {
            out_player = 0;
            if ( !plausible_ptr( m_game_base ) || !plausible_ptr( base_api ) )
                return false;

            const ULONGLONG now = GetTickCount64( );
            if ( m_last_exact_player_scan_ms != 0 && now - m_last_exact_player_scan_ms < 750 )
                return false;
            m_last_exact_player_scan_ms = now;

            MEMORY_BASIC_INFORMATION mbi{};
            if ( !VirtualQueryEx( process.handle( ), reinterpret_cast<LPCVOID>( m_game_base ), &mbi, sizeof( mbi ) ) )
                return false;
            if ( mbi.State != MEM_COMMIT )
                return false;

            const uint64_t region_start = reinterpret_cast<uint64_t>( mbi.BaseAddress );
            const uint64_t region_end = region_start + static_cast<uint64_t>( mbi.RegionSize );
            constexpr uint64_t radius = 32ull * 1024ull * 1024ull;
            const uint64_t scan_start = ( m_game_base > radius && m_game_base - radius > region_start )
                ? m_game_base - radius : region_start;
            const uint64_t scan_end = ( m_game_base + radius < region_end )
                ? m_game_base + radius : region_end;

            if ( scan_end <= scan_start )
                return false;

            if ( scan_range_for_exact_player( process, scan_start,
                    static_cast<size_t>( scan_end - scan_start ), base_api, out_player ) ) {
                dbg::log( "LAYOUT EXACT player_scan player=0x%llX game=0x%llX radius_mb=32",
                    static_cast<unsigned long long>( out_player ),
                    static_cast<unsigned long long>( m_game_base ) );
                return true;
            }

            return false;
        }

        bool resolve_exact_player_from_screen( memory::c_process& process, uint64_t screen, uint64_t base_api,
                                               uint64_t& out_player, bool& out_via_loader ) {
            out_player = 0;
            out_via_loader = false;
            if ( !plausible_ptr( screen ) )
                return false;

            if ( validate_exact_player( process, screen, base_api ) ) {
                out_player = screen;
                return true;
            }

            // PlayerLoader::CurrentPlayer is +0x428 in the exact 2026.804.2 layout.
            // During loading / transitions the stack contains PlayerLoader while the
            // real SoloPlayer is held by this field.
            const uint64_t child = process.read<uint64_t>( screen + m_off.player_loader_current_player );
            if ( validate_exact_player( process, child, base_api ) ) {
                out_player = child;
                out_via_loader = true;
                return true;
            }

            return false;
        }

        bool resolve_screen_stack_auto( memory::c_process& process, uint64_t screen_stack, uint64_t base_api,
                                        uint64_t& out_holder, sequence_layout_t& out_seq, uint64_t& out_screen ) {
            out_holder = 0;
            out_seq = {};
            out_screen = 0;
            if ( !plausible_ptr( screen_stack ) || !plausible_ptr( base_api ) )
                return false;

            struct best_t {
                bool ok = false;
                int outer = -1;
                int inner = -1;
                int api_off = -1;
                int player_index = -1;
                bool via_loader = false;
                int score = -0x3fffffff;
                uint64_t holder = 0;
                uint64_t screen = 0;
                sequence_layout_t seq{};
            } best;

            int diag_score = -1;
            int diag_outer = -1;
            int diag_inner = -1;
            uint64_t diag_holder = 0;
            uint64_t diag_top = 0;
            sequence_layout_t diag_seq{};
            bool did_broad_scan = false;

            auto inspect_sequence = [&]( uint64_t holder, int outer, int inner ) {
                if ( !plausible_ptr( holder ) || holder == m_game_base || holder == base_api )
                    return;

                const auto seq = resolve_sequence_layout( process, holder, 128 );
                if ( !seq.ok || seq.count <= 0 )
                    return;

                const uint64_t top = process.read<uint64_t>(
                    seq.items + seq.element_offset + 8ull * static_cast<uint64_t>( seq.count - 1 ) );
                if ( plausible_ptr( top ) && seq.score > diag_score ) {
                    diag_score = seq.score;
                    diag_outer = outer;
                    diag_inner = inner;
                    diag_holder = holder;
                    diag_top = top;
                    diag_seq = seq;
                }

                // Do not assume the newest/top screen is the Player. Pause/overlay
                // screens may sit above Player, so inspect every live stack entry from
                // newest to oldest and pick the first object which actually links back
                // to this Game's API.
                for ( int i = seq.count - 1; i >= 0; --i ) {
                    const uint64_t screen = process.read<uint64_t>(
                        seq.items + seq.element_offset + 8ull * static_cast<uint64_t>( i ) );
                    if ( !plausible_ptr( screen ) || screen == m_game_base || screen == base_api ||
                         screen == screen_stack || screen == holder || screen == seq.items )
                        continue;

                    uint64_t player = 0;
                    bool via_loader = false;
                    if ( !resolve_exact_player_from_screen( process, screen, base_api, player, via_loader ) )
                        continue;

                    const uint64_t api_value = process.read<uint64_t>( player + m_off.player_api );
                    const bool exact = api_value == base_api;

                    int score = seq.score * 4;
                    score += exact ? 100 : 90;
                    score += ( i == seq.count - 1 ) ? 8 : 0;
                    score += via_loader ? 2 : 6;
                    score -= inner >= 0 ? 4 : 0;
                    score -= outer >= 0 ? std::abs( outer - static_cast<int>( m_off.screen_stack_stack ) ) / 0x80 : 0;

                    if ( !best.ok || score > best.score ) {
                        best.ok = true;
                        best.outer = outer;
                        best.inner = inner;
                        best.api_off = static_cast<int>( m_off.player_api );
                        best.player_index = i;
                        best.via_loader = via_loader;
                        best.score = score;
                        best.holder = holder;
                        best.screen = player;
                        best.seq = seq;
                    }
                    break;
                }
            };

            auto resolve_cached_holder = [&]() -> uint64_t {
                if ( m_dyn_screen_stack_field < 0 )
                    return 0;

                uint64_t p = process.read<uint64_t>(
                    screen_stack + static_cast<uint32_t>( m_dyn_screen_stack_field ) );
                if ( !plausible_ptr( p ) )
                    return 0;

                if ( m_dyn_screen_stack_nested_field >= 0 ) {
                    p = process.read<uint64_t>(
                        p + static_cast<uint32_t>( m_dyn_screen_stack_nested_field ) );
                    if ( !plausible_ptr( p ) )
                        return 0;
                }
                return p;
            };

            // Fast path: reuse the discovered object path, but re-resolve the managed
            // object addresses each frame because CoreCLR may move the objects.
            if ( m_dyn_screen_stack_field >= 0 ) {
                const uint64_t holder = resolve_cached_holder( );
                if ( holder )
                    inspect_sequence( holder, m_dyn_screen_stack_field, m_dyn_screen_stack_nested_field );
            }

            if ( !best.ok ) {
                const ULONGLONG now = GetTickCount64( );
                if ( m_last_screen_layout_probe_ms != 0 && now - m_last_screen_layout_probe_ms < 500 )
                    return false;
                m_last_screen_layout_probe_ms = now;
                did_broad_scan = true;

                // The current ScreenStack implementation may wrap the actual
                // Stack/List in one additional managed object. Search both one and two
                // pointer hops instead of forcing screen_stack + offset to be Stack<T>.
                inspect_sequence( screen_stack, -1, -1 );

                std::vector<std::pair<int, uint64_t>> children;
                children.reserve( 96 );

                for ( uint32_t outer = 0x08; outer <= 0x1400; outer += 0x08 ) {
                    const uint64_t child = process.read<uint64_t>( screen_stack + outer );
                    if ( !plausible_ptr( child ) || child == screen_stack || child == m_game_base || child == base_api )
                        continue;

                    bool duplicate = false;
                    for ( const auto& c : children ) {
                        if ( c.second == child ) {
                            duplicate = true;
                            break;
                        }
                    }
                    if ( duplicate )
                        continue;

                    children.emplace_back( static_cast<int>( outer ), child );
                    inspect_sequence( child, static_cast<int>( outer ), -1 );
                    if ( children.size( ) >= 96 )
                        break;
                }

                if ( !best.ok ) {
                    for ( const auto& [outer, child] : children ) {
                        for ( uint32_t inner = 0x08; inner <= 0x700; inner += 0x08 ) {
                            const uint64_t grand = process.read<uint64_t>( child + inner );
                            if ( !plausible_ptr( grand ) || grand == child || grand == screen_stack ||
                                 grand == m_game_base || grand == base_api )
                                continue;

                            inspect_sequence( grand, outer, static_cast<int>( inner ) );
                        }
                    }
                }
            }

            if ( !best.ok ) {
                if ( did_broad_scan ) {
                    if ( diag_score >= 0 ) {
                        const uint64_t old_player = plausible_ptr( diag_top )
                            ? process.read<uint64_t>( diag_top + m_off.player_api ) : 0;
                        const uint64_t old_submit = plausible_ptr( diag_top )
                            ? process.read<uint64_t>( diag_top + m_off.submitting_player_api ) : 0;
                        const uint64_t old_ruleset = plausible_ptr( diag_top )
                            ? process.read<uint64_t>( diag_top + m_off.player_drawable_ruleset ) : 0;
                        dbg::log(
                            "GRAPH candidate outer=0x%X nested=0x%X holder=0x%llX items=0x%X count=0x%X elem=0x%X n=%d top=0x%llX old_player=0x%llX old_submit=0x%llX old_ruleset=0x%llX seqscore=%d",
                            diag_outer, diag_inner,
                            static_cast<unsigned long long>( diag_holder ),
                            diag_seq.items_offset, diag_seq.count_offset, diag_seq.element_offset,
                            diag_seq.count,
                            static_cast<unsigned long long>( diag_top ),
                            static_cast<unsigned long long>( old_player ),
                            static_cast<unsigned long long>( old_submit ),
                            static_cast<unsigned long long>( old_ruleset ),
                            diag_score );
                    } else {
                        dbg::log( "GRAPH no-sequence screen_stack=0x%llX",
                            static_cast<unsigned long long>( screen_stack ) );
                    }
                }
                return false;
            }

            const bool changed =
                m_dyn_screen_stack_field != best.outer ||
                m_dyn_screen_stack_nested_field != best.inner ||
                m_dyn_screen_api_offset != best.api_off ||
                m_dyn_stack_items_offset != static_cast<int32_t>( best.seq.items_offset ) ||
                m_dyn_stack_count_offset != static_cast<int32_t>( best.seq.count_offset ) ||
                m_dyn_array_element_offset != static_cast<int32_t>( best.seq.element_offset );

            m_dyn_screen_stack_field = best.outer;
            m_dyn_screen_stack_nested_field = best.inner;
            m_dyn_screen_api_offset = best.api_off;
            m_dyn_stack_items_offset = static_cast<int32_t>( best.seq.items_offset );
            m_dyn_stack_count_offset = static_cast<int32_t>( best.seq.count_offset );
            m_dyn_array_element_offset = static_cast<int32_t>( best.seq.element_offset );

            // The Player API field is shared by gameplay readers. The screen-stack
            // path itself can now be two hops, so keep it in the dynamic fields rather
            // than pretending one static screen_stack_stack offset can represent it.
            m_off.player_api = static_cast<uint32_t>( best.api_off );

            if ( changed ) {
                dbg::log(
                    "LAYOUT EXACT screen_stack outer=0x%X nested=0x%X items=0x%X count=0x%X elem=0x%X n=%d player_index=%d via_loader=%d player=0x%llX api_off=0x%X score=%d",
                    best.outer, best.inner,
                    best.seq.items_offset, best.seq.count_offset, best.seq.element_offset,
                    best.seq.count, best.player_index, best.via_loader ? 1 : 0,
                    static_cast<unsigned long long>( best.screen ),
                    best.api_off, best.score );
            }

            out_holder = best.holder;
            out_seq = best.seq;
            out_screen = best.screen;
            return true;
        }

        static bool looks_like_hash( const std::string& s ) {
            if ( s.size( ) != 32 && s.size( ) != 64 )
                return false;
            for ( unsigned char c : s ) {
                if ( !std::isxdigit( c ) )
                    return false;
            }
            return true;
        }

        bool try_resolve_beatmap_metadata( memory::c_process& process, uint64_t beatmap_bindable, osu::game_snapshot_t& snap ) {
            if ( !plausible_ptr( beatmap_bindable ) )
                return false;
            const ULONGLONG now = GetTickCount64( );
            if ( m_last_metadata_probe_ms != 0 && now - m_last_metadata_probe_ms < 500 )
                return false;
            m_last_metadata_probe_ms = now;

            struct best_t {
                int score = 0;
                uint32_t bind_off = 0;
                uint32_t info_off = 0;
                uint32_t hash_off = 0;
                uint32_t diff_off = 0;
                uint32_t id_off = 0;
                uint64_t working = 0;
                uint64_t info = 0;
                std::string hash;
                std::string diff;
                int32_t map_id = 0;
            } best;

            for ( uint32_t bind_off = 0x08; bind_off <= 0x100; bind_off += 0x08 ) {
                const uint64_t working = process.read<uint64_t>( beatmap_bindable + bind_off );
                if ( !plausible_ptr( working ) )
                    continue;

                for ( uint32_t info_off = 0x08; info_off <= 0x90; info_off += 0x08 ) {
                    const uint64_t info = process.read<uint64_t>( working + info_off );
                    if ( !plausible_ptr( info ) )
                        continue;

                    uint32_t found_hash_off = 0;
                    std::string found_hash;
                    for ( uint32_t str_off = 0x08; str_off <= 0x180; str_off += 0x08 ) {
                        std::string value;
                        if ( process.read_dotnet_string( info + str_off, value ) && looks_like_hash( value ) ) {
                            found_hash_off = str_off;
                            found_hash = value;
                            break;
                        }
                    }
                    if ( found_hash.empty( ) )
                        continue;

                    int score = 20;
                    std::string diff;
                    uint32_t diff_off = 0;

                    // Prefer a readable short string near the previous difficulty field.
                    int best_diff_dist = 0x7fffffff;
                    for ( uint32_t str_off = 0x08; str_off <= 0x180; str_off += 0x08 ) {
                        if ( str_off == found_hash_off )
                            continue;
                        std::string value;
                        if ( !process.read_dotnet_string( info + str_off, value ) )
                            continue;
                        if ( value.empty( ) || value.size( ) > 96 || looks_like_hash( value ) )
                            continue;
                        bool printable = true;
                        for ( unsigned char c : value ) {
                            if ( c < 0x20 && c != '\t' ) { printable = false; break; }
                        }
                        if ( !printable )
                            continue;
                        const int dist = std::abs( static_cast<int>( str_off ) - static_cast<int>( m_off.map_info_difficulty ) );
                        if ( dist < best_diff_dist ) {
                            best_diff_dist = dist;
                            diff_off = str_off;
                            diff = value;
                        }
                    }
                    if ( !diff.empty( ) ) score += 4;

                    int32_t map_id = process.read<int32_t>( info + m_off.map_info_online_id );
                    uint32_t id_off = m_off.map_info_online_id;
                    if ( map_id <= 0 || map_id >= 100000000 ) {
                        map_id = 0;
                        int best_id_dist = 0x7fffffff;
                        for ( uint32_t off = 0x20; off <= 0x160; off += 0x04 ) {
                            const int32_t v = process.read<int32_t>( info + off );
                            if ( v <= 0 || v >= 100000000 )
                                continue;
                            const int dist = std::abs( static_cast<int>( off ) - static_cast<int>( m_off.map_info_online_id ) );
                            if ( dist < best_id_dist ) {
                                best_id_dist = dist;
                                id_off = off;
                                map_id = v;
                            }
                        }
                    }
                    if ( map_id > 0 ) score += 3;
                    if ( bind_off == m_off.bindable_value ) score += 1;
                    if ( info_off == m_off.working_map_info ) score += 1;

                    if ( score > best.score ) {
                        best.score = score;
                        best.bind_off = bind_off;
                        best.info_off = info_off;
                        best.hash_off = found_hash_off;
                        best.diff_off = diff_off;
                        best.id_off = id_off;
                        best.working = working;
                        best.info = info;
                        best.hash = found_hash;
                        best.diff = diff;
                        best.map_id = map_id;
                    }
                }
            }

            if ( best.score < 20 )
                return false;

            const bool changed = m_off.bindable_value != best.bind_off || m_off.working_map_info != best.info_off ||
                m_off.map_info_hash != best.hash_off || ( best.diff_off && m_off.map_info_difficulty != best.diff_off ) ||
                ( best.map_id > 0 && m_off.map_info_online_id != best.id_off );

            m_off.bindable_value = best.bind_off;
            m_off.working_map_info = best.info_off;
            m_off.map_info_hash = best.hash_off;
            if ( best.diff_off ) m_off.map_info_difficulty = best.diff_off;
            if ( best.map_id > 0 ) m_off.map_info_online_id = best.id_off;

            snap.diag_working_beatmap = best.working;
            snap.diag_beatmap_ok = true;
            snap.beatmap_hash = best.hash;
            snap.beatmap_version = best.diff;
            snap.map_id = best.map_id;

            if ( changed ) {
                dbg::log( "LAYOUT beatmap bind=0x%X info=0x%X hash=0x%X diff=0x%X id=0x%X map=%d hashlen=%u",
                    best.bind_off, best.info_off, best.hash_off, best.diff_off, best.id_off, best.map_id,
                    static_cast<unsigned>( best.hash.size( ) ) );
            }
            return true;
        }

        bool try_dynamic_clock( memory::c_process& process, double& out_time ) {
            out_time = 0.0;

            if ( m_dyn_game_clock_offset >= 0 && m_dyn_clock_source_offset >= 0 && m_dyn_clock_time_offset >= 0 ) {
                const uint64_t clock_obj = process.read<uint64_t>( m_game_base + static_cast<uint32_t>( m_dyn_game_clock_offset ) );
                const uint64_t source = plausible_ptr( clock_obj )
                    ? process.read<uint64_t>( clock_obj + static_cast<uint32_t>( m_dyn_clock_source_offset ) ) : 0;
                if ( plausible_ptr( source ) ) {
                    const double value = process.read<double>( source + static_cast<uint32_t>( m_dyn_clock_time_offset ) );
                    if ( std::isfinite( value ) && value >= -60000.0 && value <= 86400000.0 ) {
                        out_time = value;
                        return true;
                    }
                }
            }

            const ULONGLONG now = GetTickCount64( );
            const ULONGLONG elapsed = m_last_root_clock_scan_ms == 0 ? 100 : now - m_last_root_clock_scan_ms;
            if ( m_last_root_clock_scan_ms != 0 && elapsed < 150 )
                return false;
            m_last_root_clock_scan_ms = now;

            int best_hits = m_dyn_clock_hits;
            int best_go = m_dyn_game_clock_offset;
            int best_so = m_dyn_clock_source_offset;
            int best_to = m_dyn_clock_time_offset;
            double best_value = 0.0;
            int best_distance = 0x7fffffff;
            if ( best_go >= 0 && best_so >= 0 && best_to >= 0 ) {
                best_distance = std::abs( best_go - static_cast<int>( m_off.game_base_beatmap_clock ) ) +
                    std::abs( best_so - static_cast<int>( m_off.framed_clock_final_source ) ) +
                    std::abs( best_to - static_cast<int>( m_off.framed_clock_current_time ) );
            }

            // Search around the old Game.BeatmapClock field. For each pointed-to
            // object, search pointer fields around the old FinalSource location,
            // then look for a millisecond-like double which advances in step
            // with wall time. Offsets are tracked across samples, so random
            // animation values do not survive long enough to be accepted.
            const int32_t game_center = static_cast<int32_t>( m_off.game_base_beatmap_clock );
            const int32_t game_lo = (std::max)( 0x200, game_center - 0x200 );
            const int32_t game_hi = game_center + 0x200;
            const int32_t src_center = static_cast<int32_t>( m_off.framed_clock_final_source );
            const int32_t src_lo = (std::max)( 8, src_center - 0x180 );
            const int32_t src_hi = src_center + 0x180;

            for ( int32_t go = game_lo; go <= game_hi; go += 8 ) {
                const uint64_t clock_obj = process.read<uint64_t>( m_game_base + static_cast<uint32_t>( go ) );
                if ( !plausible_ptr( clock_obj ) )
                    continue;

                for ( int32_t so = src_lo; so <= src_hi; so += 8 ) {
                    const uint64_t source = process.read<uint64_t>( clock_obj + static_cast<uint32_t>( so ) );
                    if ( !plausible_ptr( source ) )
                        continue;

                    for ( int32_t to = 0x08; to <= 0x90; to += 8 ) {
                        const double value = process.read<double>( source + static_cast<uint32_t>( to ) );
                        if ( !std::isfinite( value ) || value < -60000.0 || value > 86400000.0 )
                            continue;

                        const uint64_t key = ( static_cast<uint64_t>( go & 0xFFFF ) << 32 ) |
                                             ( static_cast<uint64_t>( so & 0xFFFF ) << 16 ) |
                                             static_cast<uint64_t>( to & 0xFFFF );
                        auto& tr = m_root_clock_tracks[ key ];
                        if ( tr.have_prev ) {
                            const double delta = value - tr.prev;
                            const double min_delta = (std::max)( 0.05, static_cast<double>( elapsed ) * 0.20 );
                            const double max_delta = (std::max)( 250.0, static_cast<double>( elapsed ) * 3.0 );
                            if ( delta >= min_delta && delta <= max_delta ) {
                                if ( tr.hits < 100000 ) ++tr.hits;
                            } else if ( delta < -1000.0 || delta > 5000.0 ) {
                                tr.hits = 0;
                            }
                        }
                        tr.prev = value;
                        tr.have_prev = true;

                        const int distance = std::abs( go - game_center ) + std::abs( so - src_center ) +
                            std::abs( to - static_cast<int>( m_off.framed_clock_current_time ) );
                        if ( value > 250.0 && ( tr.hits > best_hits || ( tr.hits == best_hits && tr.hits > 0 && distance < best_distance ) ) ) {
                            best_hits = tr.hits;
                            best_go = go;
                            best_so = so;
                            best_to = to;
                            best_value = value;
                            best_distance = distance;
                        }
                    }
                }
            }

            if ( best_hits >= 8 ) {
                const bool changed = best_go != m_dyn_game_clock_offset || best_so != m_dyn_clock_source_offset || best_to != m_dyn_clock_time_offset;
                m_dyn_game_clock_offset = best_go;
                m_dyn_clock_source_offset = best_so;
                m_dyn_clock_time_offset = best_to;
                m_dyn_clock_hits = best_hits;
                out_time = best_value;
                if ( changed || ( best_hits % 20 ) == 0 ) {
                    dbg::log( "LAYOUT clock game=0x%X source=0x%X time=0x%X hits=%d value=%d",
                        m_dyn_game_clock_offset, m_dyn_clock_source_offset, m_dyn_clock_time_offset,
                        m_dyn_clock_hits, static_cast<int32_t>( best_value ) );
                }
                return true;
            }

            return false;
        }

        bool try_exact_gameplay_clock( memory::c_process& process, uint64_t player,
                                              float speed_mult, double& out_time ) {
            out_time = 0.0;
            if ( !plausible_ptr( player ) )
                return false;

            const uint64_t container = process.read<uint64_t>(
                player + m_off.player_gameplay_clock_container );
            if ( !plausible_ptr( container ) )
                return false;

            const uint64_t framed = process.read<uint64_t>(
                container + m_off.gameplay_clock_gameplay_clock );
            if ( !plausible_ptr( framed ) )
                return false;

            auto read_latched = [&]( double& value ) -> bool {
                if ( m_exact_clock_time < 0 )
                    return false;
                uint64_t target = framed;
                if ( m_exact_clock_nested >= 0 ) {
                    target = process.read<uint64_t>( framed + static_cast<uint32_t>( m_exact_clock_nested ) );
                    if ( !plausible_ptr( target ) )
                        return false;
                }
                value = process.read<double>( target + static_cast<uint32_t>( m_exact_clock_time ) );
                return std::isfinite( value ) && value >= -60000.0 && value <= 3600000.0;
            };

            double latched = 0.0;
            if ( read_latched( latched ) ) {
                out_time = latched;
                return true;
            }

            const ULONGLONG now = GetTickCount64( );
            if ( m_last_exact_clock_scan_ms != 0 && now - m_last_exact_clock_scan_ms < 90 )
                return false;

            const ULONGLONG prev_scan = m_prev_exact_clock_scan_ms;
            m_prev_exact_clock_scan_ms = now;
            m_last_exact_clock_scan_ms = now;
            ++m_exact_clock_scan_count;

            const double wall_delta = prev_scan ? static_cast<double>( now - prev_scan ) : 0.0;
            const double speed = speed_mult > 0.2f && speed_mult < 3.0f ? speed_mult : 1.0f;
            const double expected = wall_delta * speed;

            int best_hits = 0;
            int best_nested = -2;
            int best_time = -1;
            double best_value = 0.0;
            double best_error = 1e30;

            auto feed = [&]( uint64_t key, double value, int nested_off, int time_off, double preference ) {
                if ( !std::isfinite( value ) || value < -60000.0 || value > 3600000.0 )
                    return;

                auto& tr = m_exact_clock_tracks[ key ];
                double error = 1e30;
                if ( tr.have_prev && wall_delta > 0.0 ) {
                    const double delta = value - tr.prev;
                    error = std::abs( delta - expected );
                    const double tolerance = (std::max)( 35.0, expected * 0.5 );
                    if ( delta > 0.5 && delta < 1500.0 && error <= tolerance ) {
                        tr.hits = (std::min)( tr.hits + 1, 10000 );
                    } else if ( std::abs( delta ) <= 0.5 ) {
                        // Pauses keep the candidate alive without gaining confidence.
                    } else if ( delta < -250.0 || delta > 3000.0 ) {
                        tr.hits = 0;
                    } else if ( error > tolerance * 2.0 ) {
                        tr.hits = (std::max)( 0, tr.hits - 1 );
                    }
                }
                tr.prev = value;
                tr.have_prev = true;

                double tie = std::isfinite( error ) ? error : 1e30;
                tie += preference;
                if ( tr.hits > best_hits || ( tr.hits == best_hits && tr.hits > 0 && tie < best_error ) ) {
                    best_hits = tr.hits;
                    best_nested = nested_off;
                    best_time = time_off;
                    best_value = value;
                    best_error = tie;
                }
            };

            std::array<uint8_t, 0x600> framed_buf{};
            if ( !process.read_buffer( framed, framed_buf.data( ), framed_buf.size( ) ) )
                return false;

            // CurrentTime may be cached directly on FramedBeatmapClock.
            for ( int time_off = 0x08; time_off <= 0x5F8; time_off += 8 ) {
                double value = 0.0;
                std::memcpy( &value, framed_buf.data( ) + time_off, sizeof( value ) );
                const uint64_t key = 0xFFFF00000000ull | static_cast<uint32_t>( time_off );
                feed( key, value, -1, time_off, 10.0 );
            }

            // Or it may live in one of the framed clock's source clocks. Check the
            // previously-known source field first, then a bounded set of other children.
            std::vector<int> nested_offsets;
            nested_offsets.reserve( 18 );
            if ( m_off.framed_clock_final_source >= 0x08 && m_off.framed_clock_final_source <= 0x5F8 )
                nested_offsets.push_back( static_cast<int>( m_off.framed_clock_final_source ) );
            for ( int off = 0x08; off <= 0x5F8 && nested_offsets.size( ) < 18; off += 8 ) {
                if ( off == static_cast<int>( m_off.framed_clock_final_source ) )
                    continue;
                uint64_t ptr = 0;
                std::memcpy( &ptr, framed_buf.data( ) + off, sizeof( ptr ) );
                if ( plausible_ptr( ptr ) && ptr != framed && ptr != container && ptr != player )
                    nested_offsets.push_back( off );
            }

            std::array<uint8_t, 0x240> nested_buf{};
            for ( int nested_off : nested_offsets ) {
                uint64_t nested = 0;
                std::memcpy( &nested, framed_buf.data( ) + nested_off, sizeof( nested ) );
                if ( !plausible_ptr( nested ) || nested == framed || nested == container || nested == player )
                    continue;
                if ( !process.read_buffer( nested, nested_buf.data( ), nested_buf.size( ) ) )
                    continue;

                for ( int time_off = 0x08; time_off <= 0x238; time_off += 8 ) {
                    double value = 0.0;
                    std::memcpy( &value, nested_buf.data( ) + time_off, sizeof( value ) );
                    const uint64_t key = ( static_cast<uint64_t>( static_cast<uint32_t>( nested_off ) ) << 32 ) |
                        static_cast<uint32_t>( time_off );
                    double preference = 0.0;
                    if ( nested_off == static_cast<int>( m_off.framed_clock_final_source ) )
                        preference -= 25.0;
                    if ( time_off == static_cast<int>( m_off.framed_clock_current_time ) )
                        preference -= 10.0;
                    feed( key, value, nested_off, time_off, preference );
                }
            }

            if ( best_hits >= 4 ) {
                const bool changed = best_nested != m_exact_clock_nested || best_time != m_exact_clock_time;
                m_exact_clock_nested = best_nested;
                m_exact_clock_time = best_time;
                m_exact_clock_hits = best_hits;
                out_time = best_value;
                if ( changed || ( best_hits % 20 ) == 0 ) {
                    dbg::log( "LAYOUT EXACT gameplay_clock container=0x%llX framed=0x%llX nested=0x%X time=0x%X hits=%d value=%d",
                        static_cast<unsigned long long>( container ),
                        static_cast<unsigned long long>( framed ),
                        m_exact_clock_nested, m_exact_clock_time, m_exact_clock_hits,
                        static_cast<int32_t>( best_value ) );
                }
                return true;
            }

            return false;
        }

        bool try_player_clock( memory::c_process& process, uint64_t player, int32_t ruleset_field,
                               float speed_mult, double& out_time ) {
            out_time = 0.0;
            if ( !plausible_ptr( player ) )
                return false;

            auto read_resolved = [&]( double& value ) -> bool {
                if ( m_dyn_player_clock_field < 0 || m_dyn_player_clock_time < 0 )
                    return false;
                const uint64_t child = process.read<uint64_t>(
                    player + static_cast<uint32_t>( m_dyn_player_clock_field ) );
                if ( !plausible_ptr( child ) )
                    return false;
                uint64_t target = child;
                if ( m_dyn_player_clock_nested >= 0 ) {
                    target = process.read<uint64_t>( child + static_cast<uint32_t>( m_dyn_player_clock_nested ) );
                    if ( !plausible_ptr( target ) )
                        return false;
                }
                value = process.read<double>( target + static_cast<uint32_t>( m_dyn_player_clock_time ) );
                return std::isfinite( value ) && value >= -60000.0 && value <= 1800000.0;
            };

            double resolved = 0.0;
            if ( read_resolved( resolved ) ) {
                out_time = resolved;
                return true;
            }

            const ULONGLONG now = GetTickCount64( );
            if ( m_last_player_clock_scan_ms != 0 && now - m_last_player_clock_scan_ms < 120 )
                return false;

            const ULONGLONG prev_scan = m_prev_player_clock_scan_ms;
            m_prev_player_clock_scan_ms = now;
            m_last_player_clock_scan_ms = now;
            ++m_player_clock_scan_count;
            const double wall_delta = prev_scan ? static_cast<double>( now - prev_scan ) : 0.0;
            const double expected = wall_delta * ( speed_mult > 0.2f && speed_mult < 3.0f ? speed_mult : 1.f );

            int best_hits = 0;
            int best_field = -1;
            int best_nested = -1;
            int best_time = -1;
            double best_value = 0.0;
            double best_error = 1e30;

            auto feed = [&]( uint64_t key, double value, int field_off, int nested_off, int time_off ) {
                if ( !std::isfinite( value ) || value < -60000.0 || value > 1800000.0 )
                    return;

                auto& tr = m_player_clock_tracks[ key ];
                double error = 1e30;
                if ( tr.have_prev && wall_delta > 0.0 ) {
                    const double delta = value - tr.prev;
                    error = std::abs( delta - expected );
                    const double tolerance = (std::max)( 40.0, expected * 0.45 );
                    if ( delta > 1.0 && delta < 1500.0 && error <= tolerance ) {
                        tr.hits = (std::min)( tr.hits + 1, 10000 );
                    } else if ( std::abs( delta ) <= 1.0 ) {
                        // Paused clocks are allowed to hold their score but do not gain it.
                    } else if ( delta < -250.0 || delta > 3000.0 ) {
                        tr.hits = 0;
                    } else if ( error > tolerance * 2.0 ) {
                        tr.hits = (std::max)( 0, tr.hits - 1 );
                    }
                }
                tr.prev = value;
                tr.have_prev = true;

                // Prefer candidates that have followed wall/game time for multiple
                // samples. For equal hit counts, prefer the smaller delta error and a
                // field close to DrawableRuleset/GameplayClockContainer declarations.
                double tie_error = error;
                if ( !std::isfinite( tie_error ) ) tie_error = 1e30;
                tie_error += std::abs( field_off - ruleset_field ) * 0.02;
                if ( tr.hits > best_hits || ( tr.hits == best_hits && tr.hits > 0 && tie_error < best_error ) ) {
                    best_hits = tr.hits;
                    best_field = field_off;
                    best_nested = nested_off;
                    best_time = time_off;
                    best_value = value;
                    best_error = tie_error;
                }
            };

            // Player.cs declares DrawableRuleset, HUDOverlay and GameplayClockContainer
            // next to each other. Scan that neighbourhood first; after a few samples,
            // widen to all Player instance fields if necessary.
            int32_t lo = (std::max)( 8, ruleset_field - 0x180 );
            int32_t hi = (std::min)( 0x1800, ruleset_field + 0x280 );
            if ( m_player_clock_scan_count > 20 ) {
                lo = 0x08;
                hi = 0x1800;
            }

            std::array<uint8_t, 0x800> child_buf{};
            std::array<uint8_t, 0x240> nested_buf{};

            for ( int32_t field_off = lo; field_off <= hi; field_off += 8 ) {
                const uint64_t child = process.read<uint64_t>( player + static_cast<uint32_t>( field_off ) );
                if ( !plausible_ptr( child ) || child == player || child == m_game_base )
                    continue;
                if ( !process.read_buffer( child, child_buf.data( ), child_buf.size( ) ) )
                    continue;

                // Direct doubles in the candidate child.
                for ( int time_off = 0x08; time_off <= 0x7F8; time_off += 8 ) {
                    double value = 0.0;
                    std::memcpy( &value, child_buf.data( ) + time_off, sizeof( value ) );
                    const uint64_t key = ( static_cast<uint64_t>( static_cast<uint32_t>( field_off ) ) << 32 ) |
                        ( 0xFFFFull << 16 ) | static_cast<uint32_t>( time_off );
                    feed( key, value, field_off, -1, time_off );
                }

                // One more pointer hop catches GameplayClockContainer -> gameplay/master
                // clock and DrawableRuleset -> FrameStableClock without hard-coding either.
                int nested_checked = 0;
                for ( int nested_off = 0x08; nested_off <= 0x5F8 && nested_checked < 8; nested_off += 8 ) {
                    uint64_t nested = 0;
                    std::memcpy( &nested, child_buf.data( ) + nested_off, sizeof( nested ) );
                    if ( !plausible_ptr( nested ) || nested == child || nested == player || nested == m_game_base )
                        continue;
                    if ( !process.read_buffer( nested, nested_buf.data( ), nested_buf.size( ) ) )
                        continue;
                    ++nested_checked;
                    for ( int time_off = 0x08; time_off <= 0x238; time_off += 8 ) {
                        double value = 0.0;
                        std::memcpy( &value, nested_buf.data( ) + time_off, sizeof( value ) );
                        const uint64_t key = ( static_cast<uint64_t>( static_cast<uint32_t>( field_off ) ) << 32 ) |
                            ( static_cast<uint64_t>( static_cast<uint32_t>( nested_off ) ) << 16 ) |
                            static_cast<uint32_t>( time_off );
                        feed( key, value, field_off, nested_off, time_off );
                    }
                }
            }

            if ( best_hits >= 5 ) {
                const bool changed = best_field != m_dyn_player_clock_field ||
                    best_nested != m_dyn_player_clock_nested || best_time != m_dyn_player_clock_time;
                m_dyn_player_clock_field = best_field;
                m_dyn_player_clock_nested = best_nested;
                m_dyn_player_clock_time = best_time;
                m_dyn_player_clock_hits = best_hits;
                out_time = best_value;
                if ( changed || ( best_hits % 20 ) == 0 ) {
                    dbg::log( "LAYOUT STRICT player_clock field=0x%X nested=0x%X time=0x%X hits=%d value=%d",
                        m_dyn_player_clock_field, m_dyn_player_clock_nested, m_dyn_player_clock_time,
                        m_dyn_player_clock_hits, static_cast<int32_t>( best_value ) );
                }
                return true;
            }

            return false;
        }

        bool strict_list_layout( memory::c_process& process, uint64_t list, uint64_t& items, int32_t& count ) {
            items = 0;
            count = 0;
            if ( !plausible_ptr( list ) )
                return false;
            items = process.read<uint64_t>( list + 0x08 );
            count = process.read<int32_t>( list + 0x10 );
            if ( !plausible_ptr( items ) || count <= 0 || count > 100000 )
                return false;
            const uint64_t first = process.read<uint64_t>( items + 0x10 );
            const uint64_t last = process.read<uint64_t>( items + 0x10 + 8ull * static_cast<uint64_t>( count - 1 ) );
            return plausible_ptr( first ) && plausible_ptr( last );
        }

        hit_layout_t discover_hitobject_layout( memory::c_process& process, uint64_t list ) {
            hit_layout_t out{};
            uint64_t items = 0;
            int32_t count = 0;
            if ( !strict_list_layout( process, list, items, count ) )
                return out;

            std::vector<uint64_t> objects;
            const int32_t sample_count = (std::min)( count, 16 );
            objects.reserve( static_cast<size_t>( sample_count ) );
            for ( int32_t i = 0; i < sample_count; ++i ) {
                const uint64_t obj = process.read<uint64_t>( items + 0x10 + 8ull * static_cast<uint64_t>( i ) );
                if ( plausible_ptr( obj ) )
                    objects.push_back( obj );
            }
            if ( objects.size( ) < 3 )
                return out;

            int best_time_score = -1;
            uint32_t best_start = 0;
            uint32_t best_value = 0;
            int best_valid_times = 0;

            for ( uint32_t start_off = 0x08; start_off <= 0x120; start_off += 0x08 ) {
                for ( uint32_t value_off = 0x08; value_off <= 0xA0; value_off += 0x08 ) {
                    int valid = 0;
                    int monotonic = 0;
                    double first_time = 0.0;
                    double last_time = 0.0;
                    double prev_time = 0.0;
                    bool have_prev = false;

                    for ( uint64_t obj : objects ) {
                        const uint64_t bind = process.read<uint64_t>( obj + start_off );
                        if ( !plausible_ptr( bind ) )
                            continue;
                        const double t = process.read<double>( bind + value_off );
                        if ( !std::isfinite( t ) || t < -60000.0 || t > 3600000.0 )
                            continue;

                        if ( valid == 0 )
                            first_time = t;
                        last_time = t;
                        if ( have_prev && t + 0.5 >= prev_time )
                            ++monotonic;
                        prev_time = t;
                        have_prev = true;
                        ++valid;
                    }

                    if ( valid < 3 || monotonic < valid - 2 )
                        continue;
                    const double span = last_time - first_time;
                    if ( span < 20.0 )
                        continue;

                    int score = valid * 10 + monotonic * 5;
                    if ( span > 250.0 ) score += 20;
                    if ( start_off == m_off.hit_object_start_time_bindable ) score += 2;
                    if ( value_off == m_off.bindable_number_value ) score += 2;

                    if ( score > best_time_score ) {
                        best_time_score = score;
                        best_start = start_off;
                        best_value = value_off;
                        best_valid_times = valid;
                    }
                }
            }

            if ( best_time_score < 0 )
                return out;

            int best_pos_score = -1;
            uint32_t best_pos = 0;
            for ( uint32_t pos_off = 0x10; pos_off <= 0x180; pos_off += 0x04 ) {
                int valid = 0;
                float min_x = 99999.f, max_x = -99999.f;
                float min_y = 99999.f, max_y = -99999.f;
                for ( uint64_t obj : objects ) {
                    const float x = process.read<float>( obj + pos_off );
                    const float y = process.read<float>( obj + pos_off + 4 );
                    if ( !std::isfinite( x ) || !std::isfinite( y ) )
                        continue;
                    if ( x < -128.f || x > 640.f || y < -128.f || y > 512.f )
                        continue;
                    ++valid;
                    min_x = (std::min)( min_x, x ); max_x = (std::max)( max_x, x );
                    min_y = (std::min)( min_y, y ); max_y = (std::max)( max_y, y );
                }
                if ( valid < 3 )
                    continue;
                const float spread = ( max_x - min_x ) + ( max_y - min_y );
                if ( spread < 2.f )
                    continue;
                int score = valid * 10 + static_cast<int>( (std::min)( spread, 500.f ) / 10.f );
                if ( pos_off == m_off.osu_hit_object_position_xy ) score += 2;
                if ( score > best_pos_score ) {
                    best_pos_score = score;
                    best_pos = pos_off;
                }
            }

            if ( best_pos_score < 0 )
                return out;

            out.ok = true;
            out.start_bindable_offset = best_start;
            out.bindable_number_offset = best_value;
            out.position_offset = best_pos;
            out.valid_times = best_valid_times;
            out.score = best_time_score + best_pos_score;
            return out;
        }

        bool validate_ruleset_candidate( memory::c_process& process, uint64_t ruleset,
                                         int32_t& beatmap_off, int32_t& hitobjects_off, hit_layout_t& hit_layout ) {
            beatmap_off = -1;
            hitobjects_off = -1;
            hit_layout = {};
            if ( !plausible_ptr( ruleset ) )
                return false;

            // First stay close to the previous known fields. Layouts generally move by
            // a handful of references between osu! releases, and the hit-object timing
            // validation below is strong enough to reject unrelated List<T> instances.
            const int32_t beat_center = static_cast<int32_t>( m_off.drawable_osu_beatmap );
            const int32_t beat_lo = (std::max)( 8, beat_center - 0x180 );
            const int32_t beat_hi = beat_center + 0x180;
            const int32_t hit_center = static_cast<int32_t>( m_off.beatmap_hit_objects );
            const int32_t hit_lo = (std::max)( 8, hit_center - 0x100 );
            const int32_t hit_hi = hit_center + 0x100;

            hit_layout_t best_hit{};
            int best_bo = -1, best_ho = -1;

            for ( int32_t bo = beat_lo; bo <= beat_hi; bo += 8 ) {
                const uint64_t beatmap = process.read<uint64_t>( ruleset + static_cast<uint32_t>( bo ) );
                if ( !plausible_ptr( beatmap ) )
                    continue;

                for ( int32_t ho = hit_lo; ho <= hit_hi; ho += 8 ) {
                    const uint64_t list = process.read<uint64_t>( beatmap + static_cast<uint32_t>( ho ) );
                    uint64_t items = 0;
                    int32_t count = 0;
                    if ( !strict_list_layout( process, list, items, count ) )
                        continue;

                    const auto h = discover_hitobject_layout( process, list );
                    if ( !h.ok )
                        continue;

                    int score = h.score;
                    score -= std::abs( bo - beat_center ) / 8;
                    score -= std::abs( ho - hit_center ) / 8;
                    if ( !best_hit.ok || score > best_hit.score ) {
                        best_hit = h;
                        best_hit.score = score;
                        best_bo = bo;
                        best_ho = ho;
                    }
                }
            }

            if ( !best_hit.ok )
                return false;

            beatmap_off = best_bo;
            hitobjects_off = best_ho;
            hit_layout = best_hit;
            return true;
        }

        bool verify_game_identity( memory::c_process& process, uint64_t addr ) {
            if ( !plausible_ptr( addr ) )
                return false;

            // Preferred 2026.804.2 identity: an exact Player structurally points to this
            // Game. This does not depend on the stale Game->API offset.
            if ( validate_structural_player( process, m_exact_player_hint ) &&
                 process.read<uint64_t>( m_exact_player_hint + m_off.player_game ) == addr )
                return true;

            const auto api = process.read<uint64_t>( addr + m_off.game_base_api );
            if ( !plausible_ptr( api ) )
                return false;

            const auto api_game = process.read<uint64_t>( api + m_off.api_access_game );
            return api_game == addr;
        }

        bool verify_game_base( memory::c_process& process, uint64_t addr ) {
            // The API <-> Game back-reference is the stable identity check. The old
            // screen-stack and selected-beatmap fields moved in 2026.804.2 and must not
            // reject an otherwise valid Game object.
            return verify_game_identity( process, addr );
        }

        void reset_probe_history_after_reacquire( ) {
            m_clock_probe_prev.fill( 0.0 );
            m_clock_probe_hits.fill( 0 );
            m_clock_chain_prev.fill( 0.0 );
            m_clock_chain_hits.fill( 0 );
            m_clock_chain_best_source_offset = -1;
            m_clock_chain_best_time_offset = -1;
            m_clock_chain_best_hits = 0;
            m_clock_chain_best_value = 0;
            m_last_clock_probe_offset = -1;
            m_last_clock_probe_hits = 0;
            m_last_clock_probe_value = 0;
            m_root_clock_tracks.clear( );
            m_last_root_clock_scan_ms = 0;
            m_dyn_clock_hits = 0;
            m_player_clock_tracks.clear( );
            m_last_player_clock_scan_ms = 0;
            m_prev_player_clock_scan_ms = 0;
            m_dyn_player_clock_field = -1;
            m_dyn_player_clock_nested = -1;
            m_dyn_player_clock_time = -1;
            m_dyn_player_clock_hits = 0;
            m_player_clock_scan_count = 0;
            m_exact_clock_tracks.clear( );
            m_last_exact_clock_scan_ms = 0;
            m_prev_exact_clock_scan_ms = 0;
            m_exact_clock_nested = -2;
            m_exact_clock_time = -1;
            m_exact_clock_hits = 0;
            m_exact_clock_scan_count = 0;
            m_exact_player_hint = 0;
            m_last_exact_player_scan_ms = 0;
            m_last_exact_layout_log_ms = 0;
        }

        bool ensure_game_base( memory::c_process& process ) {
            if ( verify_game_identity( process, m_game_base ) )
                return true;

            const ULONGLONG now = GetTickCount64( );
            if ( now - m_last_reacquire_ms < 300 )
                return false;
            m_last_reacquire_ms = now;

            const uint64_t old_base = m_game_base;

            // First retry the exact pointer slot which originally led us to the
            // Game object. If it is a static/GC-updated root this is very cheap.
            if ( plausible_ptr( m_game_root_slot ) ) {
                const uint64_t candidate = process.read<uint64_t>( m_game_root_slot );
                if ( plausible_ptr( candidate ) && try_game_chain( process, candidate, m_game_root_slot ) ) {
                    if ( m_game_base != old_base ) {
                        ++m_reacquire_count;
                        reset_probe_history_after_reacquire( );
                        dbg::log( "BASE reacquired via root old=0x%llX new=0x%llX root=0x%llX count=%u",
                            static_cast<unsigned long long>( old_base ),
                            static_cast<unsigned long long>( m_game_base ),
                            static_cast<unsigned long long>( m_game_root_slot ),
                            m_reacquire_count );
                    }
                    return true;
                }
            }

            // The cached raw managed pointer can become stale after a compacting
            // GC. Rescan module roots first, then fall back to private memory.
            m_game_base = 0;
            const wchar_t* mods[] = {
                L"osu.Game.dll",
                L"osu.Framework.dll",
                L"osu.Game.Rulesets.Osu.dll",
                L"osu.Game.Rulesets.dll",
            };
            for ( auto mod_name : mods ) {
                if ( scan_module_for_game( process, mod_name ) ) {
                    ++m_reacquire_count;
                    reset_probe_history_after_reacquire( );
                    dbg::log( "BASE reacquired by module scan old=0x%llX new=0x%llX root=0x%llX count=%u",
                        static_cast<unsigned long long>( old_base ),
                        static_cast<unsigned long long>( m_game_base ),
                        static_cast<unsigned long long>( m_game_root_slot ),
                        m_reacquire_count );
                    return true;
                }
            }

            if ( scan_private_memory_for_game( process ) ) {
                ++m_reacquire_count;
                reset_probe_history_after_reacquire( );
                dbg::log( "BASE reacquired by private scan old=0x%llX new=0x%llX root=0x%llX count=%u",
                    static_cast<unsigned long long>( old_base ),
                    static_cast<unsigned long long>( m_game_base ),
                    static_cast<unsigned long long>( m_game_root_slot ),
                    m_reacquire_count );
                return true;
            }

            dbg::log( "BASE lost old=0x%llX root=0x%llX; reacquire failed",
                static_cast<unsigned long long>( old_base ),
                static_cast<unsigned long long>( m_game_root_slot ) );
            return false;
        }

        bool try_game_chain( memory::c_process& process, uint64_t candidate, uint64_t candidate_slot ) {
            if ( !plausible_ptr( candidate ) )
                return false;

            {
                const auto api = process.read<uint64_t>( candidate + m_off.ext_link_opener_api );
                if ( plausible_ptr( api ) ) {
                    const auto game = process.read<uint64_t>( api + m_off.api_access_game );
                    if ( verify_game_base( process, game ) ) {
                        m_game_base = game;
                        m_game_root_slot = candidate_slot;
                        return true;
                    }
                }
            }

            if ( verify_game_base( process, candidate ) ) {
                m_game_base = candidate;
                m_game_root_slot = candidate_slot;
                return true;
            }

            {
                const auto game = process.read<uint64_t>( candidate + m_off.api_access_game );
                if ( plausible_ptr( game ) && verify_game_base( process, game ) ) {
                    m_game_base = game;
                    m_game_root_slot = candidate_slot;
                    return true;
                }
            }

            return false;
        }

        bool scan_range_for_game( memory::c_process& process, uint64_t start, size_t len ) {
            if ( len < 8 )
                return false;

            constexpr size_t chunk_size = 8 * 1024 * 1024;
            std::vector<uint8_t> buffer;

            for ( size_t offset = 0; offset < len; offset += chunk_size ) {
                size_t to_read = (std::min)( len - offset, chunk_size );
                if ( to_read < 8 )
                    break;

                buffer.resize( to_read );
                if ( !process.read_buffer( start + offset, buffer.data( ), to_read ) )
                    continue;

                for ( size_t i = 0; i <= to_read - 8; i += 8 ) {
                    uint64_t candidate;
                    std::memcpy( &candidate, &buffer[ i ], 8 );
                    if ( candidate < 0x10000 || candidate > 0x7FFFFFFFFFFF )
                        continue;
                    if ( candidate & 7 )
                        continue;
                    if ( try_game_chain( process, candidate, start + offset + i ) )
                        return true;
                }
            }
            return false;
        }

        bool scan_module_for_game( memory::c_process& process, const wchar_t* mod_name ) {
            const auto mod_base = get_module_base( process.pid( ), mod_name );
            if ( !mod_base )
                return false;

            MEMORY_BASIC_INFORMATION mbi{};
            uint64_t scan_addr = mod_base;
            uint64_t module_end = mod_base + 0x1000000;
            while ( scan_addr < module_end ) {
                if ( !VirtualQueryEx( process.handle( ), reinterpret_cast<LPCVOID>( scan_addr ), &mbi, sizeof( mbi ) ) )
                    break;
                if ( mbi.State == MEM_COMMIT &&
                     ( mbi.Protect & ( PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE ) ) ) {
                    if ( scan_range_for_game( process, reinterpret_cast<uint64_t>( mbi.BaseAddress ),
                             (std::min)( mbi.RegionSize, (size_t)0x200000 ) ) )
                        return true;
                }
                scan_addr = reinterpret_cast<uint64_t>( mbi.BaseAddress ) + mbi.RegionSize;
            }
            return false;
        }

        bool scan_private_memory_for_game( memory::c_process& process ) {
            MEMORY_BASIC_INFORMATION mbi{};
            uint64_t addr = 0;
            while ( VirtualQueryEx( process.handle( ), reinterpret_cast<LPCVOID>( addr ), &mbi, sizeof( mbi ) ) ) {
                if ( mbi.State == MEM_COMMIT &&
                     mbi.Type == MEM_PRIVATE &&
                     ( mbi.Protect & ( PAGE_READWRITE | PAGE_EXECUTE_READWRITE ) ) &&
                     mbi.RegionSize >= 0x10000 ) {
                    if ( scan_range_for_game( process, reinterpret_cast<uint64_t>( mbi.BaseAddress ), mbi.RegionSize ) )
                        return true;
                }
                addr = reinterpret_cast<uint64_t>( mbi.BaseAddress ) + mbi.RegionSize;
            }
            return false;
        }
    };

}