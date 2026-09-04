#pragma once

#include <imgui.h>
#include <imgui_internal.h>
#include <impl/ui/theme.hxx>
#include <impl/ui/animations.hxx>
#include <windows.h>
#include <string>
#include <unordered_map>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <random>

namespace ui {

    inline void draw_glass_card( ImDrawList* dl, const ImVec2& p0, const ImVec2& p1, float rounding = 8.0f, ImU32 header_col = 0 ) {
        dl->AddRectFilled( ImVec2( p0.x + 2, p0.y + 2 ), ImVec2( p1.x + 2, p1.y + 2 ), IM_COL32( 0, 0, 0, 40 ), rounding );

        dl->AddRectFilled( p0, p1, IM_COL32( 12, 12, 20, 180 ), rounding );

        const float frost_h = ( p1.y - p0.y ) * 0.15f;
        dl->AddRectFilledMultiColor(
            p0, ImVec2( p1.x, p0.y + frost_h ),
            IM_COL32( 255, 255, 255, 6 ), IM_COL32( 255, 255, 255, 6 ),
            IM_COL32( 0, 0, 0, 0 ), IM_COL32( 0, 0, 0, 0 ) );

        dl->AddRect( p0, p1, IM_COL32( 255, 255, 255, 18 ), rounding, 0, 1.0f );

        if ( header_col ) {
            const float hw = p1.x - p0.x - 8;
            dl->AddRectFilled( ImVec2( p0.x + 4, p0.y + 1 ), ImVec2( p0.x + 4 + hw, p0.y + 1.5f ), header_col, 0.5f );
        }
    }



    struct s_bind_entry {
        std::string key;
        bool waiting = false;
        int start_frame = -1;
    };

    inline std::unordered_map<std::string, s_bind_entry> g_binds;

    inline void init_bind( const char* id, const char* default_key ) {
        if ( g_binds.find( id ) == g_binds.end( ) )
            g_binds[ id ] = { default_key ? default_key : "", false, -1 };
    }

    inline bool is_any_bind_waiting( ) {
        for ( auto& [ id, e ] : g_binds )
            if ( e.waiting ) return true;
        return false;
    }

    inline bool is_bind_waiting( const char* id ) {
        const auto it = g_binds.find( id );
        return it != g_binds.end( ) && it->second.waiting;
    }

    inline void start_bind_waiting( const char* id ) {
        for ( auto& [ bid, e ] : g_binds ) e.waiting = false;
        const auto it = g_binds.find( id );
        if ( it != g_binds.end( ) ) {
            it->second.waiting = true;
            it->second.start_frame = ImGui::GetFrameCount( );
        }
    }

    inline const char* get_bind_display( const char* id ) {
        static char buf[ 48 ];
        const auto it = g_binds.find( id );
        if ( it == g_binds.end( ) ) return "";
        if ( it->second.waiting ) return "press key";
        if ( it->second.key.empty( ) ) return "";
        snprintf( buf, sizeof( buf ), "[%s]", it->second.key.c_str( ) );
        return buf;
    }

    inline void update_bind_capture( ) {
        for ( auto& [ id, e ] : g_binds ) {
            if ( !e.waiting ) continue;
            if ( ImGui::GetFrameCount( ) <= e.start_frame + 1 ) continue;

            ImGuiIO& io = ImGui::GetIO( );
            if ( ImGui::IsKeyPressed( ImGuiKey_Escape ) ) { e.waiting = false; return; }

            if ( io.MouseClicked[ 0 ] ) { e.key = "M1"; e.waiting = false; return; }
            if ( io.MouseClicked[ 1 ] ) { e.key = "M2"; e.waiting = false; return; }
            if ( io.MouseClicked[ 2 ] ) { e.key = "M3"; e.waiting = false; return; }
            if ( io.MouseClicked[ 3 ] ) { e.key = "M4"; e.waiting = false; return; }
            if ( io.MouseClicked[ 4 ] ) { e.key = "M5"; e.waiting = false; return; }

            for ( int k = static_cast<int>( ImGuiKey_NamedKey_BEGIN ); k < static_cast<int>( ImGuiKey_NamedKey_END ); ++k ) {
                const auto key = static_cast<ImGuiKey>( k );
                if ( key == ImGuiKey_Escape || !ImGui::IsKeyPressed( key ) ) continue;

                const char* raw = ImGui::GetKeyName( key );
                std::string name = raw;
                if ( name == "LeftAlt" || name == "RightAlt" ) name = "ALT";
                else if ( name == "LeftCtrl" || name == "RightCtrl" ) name = "CTRL";
                else if ( name == "LeftShift" || name == "RightShift" ) name = "SHIFT";
                else if ( name == "LeftSuper" || name == "RightSuper" ) name = "WIN";
                else if ( name == "MouseLeft" ) name = "M1";
                else if ( name == "MouseRight" ) name = "M2";
                else if ( name == "MouseMiddle" ) name = "M3";
                else {
                    for ( auto& c : name ) c = static_cast<char>( toupper( static_cast<unsigned char>( c ) ) );
                }
                e.key = name;
                e.waiting = false;
                return;
            }
        }
    }

    inline bool checkbox( const char* label, bool* v, const char* bind_id = nullptr, const char* default_bind = nullptr, float col_w = 226.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );
        const float width = ( col_w > 0.f ) ? col_w : ImGui::GetContentRegionAvail( ).x;

        const float row_h = 22.0f;
        const float switch_w = 32.0f;
        const float switch_h = 16.0f;

        if ( bind_id ) init_bind( bind_id, default_bind );

        float bind_w = 0.f;
        if ( bind_id ) {
            const char* bd = get_bind_display( bind_id );
            bind_w = ImGui::CalcTextSize( bd ).x + 6.f;
        }

        const float cb_row_w = width - bind_w;
        ImGui::SetCursorScreenPos( pos );
        ImGui::InvisibleButton( label, ImVec2( cb_row_w, row_h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool clicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );
        if ( clicked ) *v = !( *v );

        static std::unordered_map<std::string, float> anims;
        float& anim = anims[ label ];
        anim += ( ( *v ? 1.0f : 0.0f ) - anim ) * std::min( g_delta_time * 10.0f, 1.0f );

        const ImVec2 sp0 = ImVec2( pos.x, pos.y + ( row_h - switch_h ) * 0.5f );
        const ImVec2 sp1 = ImVec2( pos.x + switch_w, sp0.y + switch_h );
        const float radius = switch_h * 0.5f;

        const ImU32 track_col = theme::lerp_color(
            IM_COL32( 25, 25, 45, 200 ), theme::accent_alpha( 180 ), anim );

        if ( *v || anim > 0.01f ) {
            const ImU32 glow_col = IM_COL32( 107, 197, 255, static_cast<int>( anim * 25 ) );
            dl->AddRectFilled(
                ImVec2( sp0.x - 4, sp0.y - 4 ),
                ImVec2( sp1.x + 4, sp1.y + 4 ),
                glow_col, radius + 4 );
        }

        dl->AddRectFilled( sp0, sp1, track_col, radius );
        dl->AddRect( sp0, sp1, theme::lerp_color( IM_COL32( 40, 40, 60, 200 ), theme::accent_alpha( 100 ), anim ), radius, 0, 1.0f );

        const float handle_r = ( switch_h - 4.0f ) * 0.5f;
        const float cx_min = sp0.x + 2.0f + handle_r;
        const float cx_max = sp1.x - 2.0f - handle_r;
        const float cx = cx_min + ( cx_max - cx_min ) * anim;
        const float cy = sp0.y + switch_h * 0.5f;

        dl->AddCircleFilled( ImVec2( cx, cy ), handle_r, theme::handle(), 16 );

        const float tly = pos.y + ( row_h - ImGui::GetTextLineHeight( ) ) * 0.5f;
        dl->AddText( ImVec2( pos.x + switch_w + 10.f, tly ), hov ? theme::text_bright() : theme::text(), label );

        if ( bind_id ) {
            const bool waiting = is_bind_waiting( bind_id );
            const char* bd = get_bind_display( bind_id );
            const ImVec2 bts = ImGui::CalcTextSize( bd );
            const float bx = pos.x + width - bts.x;

            const std::string btn_id = std::string( "##bnd_" ) + bind_id;
            ImGui::SetCursorScreenPos( ImVec2( bx - 3.f, pos.y ) );
            ImGui::InvisibleButton( btn_id.c_str( ), ImVec2( bts.x + 6.f, row_h ) );
            const bool bhov = ImGui::IsItemHovered( );
            const bool bclicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );

            if ( bclicked ) {
                if ( waiting ) g_binds[ bind_id ].waiting = false;
                else start_bind_waiting( bind_id );
            }

            static std::unordered_map<std::string, float> pulse;
            float& ph = pulse[ bind_id ];
            if ( waiting ) ph = fmodf( ph + g_delta_time * 4.f, 6.2832f );

            const ImU32 bc = waiting
                ? theme::accent_alpha( 0.55f + 0.45f * sinf( ph ) )
                : ( bhov ? theme::text_accent() : theme::text_dim() );

            dl->AddText( ImVec2( bx, tly ), bc, bd );
        }

        ImGui::SetCursorScreenPos( ImVec2( pos.x, pos.y + row_h + 4.0f ) );
        return clicked;
    }

    inline bool slider_float( const char* label, float* v, float vmin, float vmax, const char* suffix = "", const char* fmt = "%.0f", float col_w = 226.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );
        const float avail_w = ( col_w > 0.f ) ? col_w : ImGui::GetContentRegionAvail( ).x;

        const float lbl_h = ImGui::GetTextLineHeight( );
        const float track_h = 4.0f;
        const float total_h = lbl_h + 8.0f + track_h + 6.0f;

        dl->AddText( pos, theme::text(), label );

        const ImVec2 tp0 = ImVec2( pos.x, pos.y + lbl_h + 6.0f );
        const ImVec2 tp1 = ImVec2( tp0.x + avail_w, tp0.y + track_h );

        const std::string id = std::string( "##sl" ) + label;
        ImGui::SetCursorScreenPos( ImVec2( tp0.x, tp0.y - 6.0f ) );
        ImGui::InvisibleButton( id.c_str( ), ImVec2( avail_w, track_h + 12.0f ) );
        const bool held = ImGui::IsItemActive( );
        const bool hov = ImGui::IsItemHovered( );
        bool changed = false;

        if ( held ) {
            const float mx = ImGui::GetIO( ).MousePos.x;
            const float t = ImClamp( ( mx - tp0.x ) / avail_w, 0.0f, 1.0f );
            const float nv = vmin + t * ( vmax - vmin );
            if ( fabsf( nv - *v ) > 1e-5f ) { *v = nv; changed = true; }
        }

        const float t = ( vmax > vmin ) ? ImClamp( ( *v - vmin ) / ( vmax - vmin ), 0.0f, 1.0f ) : 0.0f;

        dl->AddRectFilled( tp0, tp1, theme::track_bg(), track_h * 0.5f );

        if ( t > 0.001f ) {
            const ImVec2 fe = ImVec2( tp0.x + t * avail_w, tp1.y );
            dl->AddRectFilled( tp0, fe, theme::track_fill(), track_h * 0.5f );
        }

        const float handle_r = 6.0f;
        const ImVec2 handle_pos = ImVec2( tp0.x + t * avail_w, tp0.y + track_h * 0.5f );

        dl->AddCircleFilled( handle_pos, handle_r, theme::handle(), 16 );
        dl->AddCircle( handle_pos, handle_r, theme::accent_alpha( held ? 200.f : 120.f ), 16, 1.5f );

        char buf[ 32 ], full[ 48 ];
        snprintf( buf, sizeof( buf ), fmt, *v );
        snprintf( full, sizeof( full ), "%s%s", buf, suffix );
        const ImVec2 text_size = ImGui::CalcTextSize( full );
        dl->AddText( ImVec2( pos.x + avail_w - text_size.x, pos.y ), theme::text_accent(), full );

        ImGui::SetCursorScreenPos( ImVec2( pos.x, pos.y + total_h ) );
        return changed;
    }

    inline bool slider_int( const char* label, int* v, int vmin, int vmax, const char* suffix = "", float col_w = 226.0f ) {
        float fv = static_cast<float>( *v );
        if ( slider_float( label, &fv, static_cast<float>( vmin ), static_cast<float>( vmax ), suffix, "%.0f", col_w ) ) {
            *v = static_cast<int>( std::round( fv ) );
            return true;
        }
        return false;
    }

    namespace _dd {
        inline ImGuiID open_id = 0;
        inline int open_frame = -1;
        inline ImVec2 open_pos = {};
        inline float open_w = 0.f;
        inline const char** items = nullptr;
        inline int count = 0;
        inline int* selected = nullptr;
    }

    inline bool dropdown( const char* label, int* sel, const char** items, int count, float col_w = 226.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );

        const float h = 22.0f;
        const float w = ( col_w > 0.f ) ? col_w : ImGui::GetContentRegionAvail( ).x;

        ImGui::SetCursorScreenPos( pos );
        ImGui::InvisibleButton( label, ImVec2( w, h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool clicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );

        const ImGuiID my_id = ImGui::GetID( label );

        if ( clicked ) {
            if ( _dd::open_id == my_id ) _dd::open_id = 0;
            else {
                _dd::open_id = my_id;
                _dd::open_frame = ImGui::GetFrameCount( );
                _dd::open_pos = ImVec2( pos.x, pos.y + h );
                _dd::open_w = w;
                _dd::items = items;
                _dd::count = count;
                _dd::selected = sel;
            }
        }

        const bool is_open = ( _dd::open_id == my_id );

        dl->AddRectFilled( pos, ImVec2( pos.x + w, pos.y + h ), IM_COL32( 10, 10, 18, 220 ), 4.0f );
        dl->AddRect( pos, ImVec2( pos.x + w, pos.y + h ), hov ? IM_COL32( 255, 255, 255, 18 ) : IM_COL32( 30, 30, 50, 150 ), 4.0f, 0, 1.0f );

        dl->AddRectFilledMultiColor( pos, ImVec2( pos.x + w, pos.y + h * 0.5f ),
            IM_COL32( 255, 255, 255, 6 ), IM_COL32( 255, 255, 255, 6 ), IM_COL32( 0, 0, 0, 0 ), IM_COL32( 0, 0, 0, 0 ) );

        const char* cur = ( *sel >= 0 && *sel < count ) ? items[ *sel ] : "";
        const float ty = pos.y + ( h - ImGui::GetTextLineHeight( ) ) * 0.5f;
        dl->AddText( ImVec2( pos.x + 8.0f, ty ), theme::text(), cur );

        const float ax = pos.x + w - 14.0f;
        const float ay = pos.y + h * 0.5f;
        dl->AddTriangleFilled(
            ImVec2( ax, ay - 2.0f ),
            ImVec2( ax + 5.0f, ay - 2.0f ),
            ImVec2( ax + 2.5f, ay + 2.0f ),
            is_open ? theme::accent() : theme::text_dim() );

        ImGui::SetCursorScreenPos( ImVec2( pos.x, pos.y + h + 4.0f ) );
        return clicked;
    }

    inline void render_open_dropdown( ) {
        if ( _dd::open_id == 0 ) return;

        ImDrawList* fdl = ImGui::GetForegroundDrawList( );
        const float item_h = 20.0f;
        const float total = static_cast<float>( _dd::count ) * item_h + 4.0f;
        const ImVec2 p0 = _dd::open_pos;
        const ImVec2 p1 = ImVec2( p0.x + _dd::open_w, p0.y + total );
        const ImVec2 mouse = ImGui::GetIO( ).MousePos;

        fdl->AddRectFilled( ImVec2( p0.x + 2, p0.y + 2 ), ImVec2( p1.x + 2, p1.y + 2 ), IM_COL32( 0, 0, 0, 80 ), 6.0f );

        fdl->AddRectFilled( p0, p1, IM_COL32( 10, 10, 18, 220 ), 6.0f );
        fdl->AddRect( p0, p1, IM_COL32( 255, 255, 255, 18 ), 6.0f, 0, 1.0f );
        fdl->AddRectFilledMultiColor( p0, ImVec2( p1.x, p0.y + total * 0.3f ),
            IM_COL32( 255, 255, 255, 6 ), IM_COL32( 255, 255, 255, 6 ), IM_COL32( 0, 0, 0, 0 ), IM_COL32( 0, 0, 0, 0 ) );

        int click_sel = -1;
        const bool click_any = ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && ( ImGui::GetFrameCount( ) > _dd::open_frame );

        for ( int i = 0; i < _dd::count; i++ ) {
            const ImVec2 ip0 = ImVec2( p0.x + 1.0f, p0.y + 2.0f + static_cast<float>( i ) * item_h );
            const ImVec2 ip1 = ImVec2( p1.x - 1.0f, ip0.y + item_h );

            const bool hov_i = ( mouse.x >= ip0.x && mouse.x <= ip1.x && mouse.y >= ip0.y && mouse.y <= ip1.y );
            const bool sel_i = ( *_dd::selected == i );

            if ( hov_i ) {
                fdl->AddRectFilled( ip0, ip1, theme::tab_hover_bg(), 3.0f );
                fdl->AddRectFilled( ImVec2( ip0.x, ip0.y + 3 ), ImVec2( ip0.x + 2, ip1.y - 3 ), theme::accent_alpha( 150 ), 1.0f );
            }

            const float ty = ip0.y + ( item_h - ImGui::GetTextLineHeight( ) ) * 0.5f;
            fdl->AddText( ImVec2( ip0.x + 8.0f, ty ), sel_i ? theme::text_accent() : ( hov_i ? theme::text_bright() : theme::text() ), _dd::items[ i ] );

            if ( hov_i && click_any ) click_sel = i;
        }

        if ( click_sel >= 0 ) {
            *_dd::selected = click_sel;
            _dd::open_id = 0;
            return;
        }

        if ( click_any ) {
            if ( !( mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y ) )
                _dd::open_id = 0;
        }
    }

    inline bool button( const char* label, float w, float h = 22.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );
        const ImVec2 p1 = ImVec2( pos.x + w, pos.y + h );

        ImGui::InvisibleButton( label, ImVec2( w, h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool held = ImGui::IsItemActive( );
        const bool clicked = ImGui::IsItemClicked( );

        ImU32 bg, bg_top, border;
        if ( held ) {
            bg = IM_COL32( 107, 197, 255, 40 );
            bg_top = IM_COL32( 107, 197, 255, 25 );
            border = theme::accent_alpha( 180 );
        }
        else if ( hov ) {
            bg = IM_COL32( 107, 197, 255, 25 );
            bg_top = IM_COL32( 107, 197, 255, 15 );
            border = theme::accent_alpha( 100 );
        }
        else {
            bg = IM_COL32( 15, 15, 35, 200 );
            bg_top = IM_COL32( 20, 20, 40, 150 );
            border = IM_COL32( 40, 40, 60, 150 );
        }

        dl->AddRectFilled( ImVec2( pos.x + 1, pos.y + 1 ), ImVec2( p1.x + 1, p1.y + 1 ), IM_COL32( 0, 0, 0, 40 ), 5.0f );

        dl->AddRectFilled( pos, p1, bg, 5.0f );
        dl->AddRectFilledMultiColor( pos, p1, bg_top, bg_top, bg, bg );
        dl->AddRect( pos, p1, border, 5.0f, 0, 1.0f );

        if ( hov && !held ) {
            dl->AddRectFilled( ImVec2( pos.x + 1, pos.y + 4 ), ImVec2( pos.x + 2, p1.y - 4 ), theme::accent_alpha( 150 ), 1.0f );
        }

        const ImVec2 ts = ImGui::CalcTextSize( label );
        const float tx = pos.x + ( w - ts.x ) * 0.5f;
        const float ty = pos.y + ( h - ts.y ) * 0.5f;
        dl->AddText( ImVec2( tx, ty ), hov ? theme::text_bright() : theme::text(), label );

        return clicked;
    }

    namespace _ti {
        inline ImGuiID focused_id = 0;
    }

    inline bool text_input( const char* id, char* buf, int buf_size, float w, float h = 22.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 pos = ImGui::GetCursorScreenPos( );
        const ImGuiID my_id = ImGui::GetID( id );
        const ImVec2 p1 = ImVec2( pos.x + w, pos.y + h );

        ImGui::InvisibleButton( id, ImVec2( w, h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool clicked = ImGui::IsItemClicked( );

        if ( clicked ) _ti::focused_id = ( _ti::focused_id == my_id ) ? 0 : my_id;
        if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) && !hov && _ti::focused_id == my_id ) _ti::focused_id = 0;

        const bool focused = ( _ti::focused_id == my_id );
        bool changed = false;

        if ( focused ) {
            ImGuiIO& io = ImGui::GetIO( );
            io.WantCaptureKeyboard = true;
            int len = static_cast<int>( strlen( buf ) );

            if ( ImGui::IsKeyPressed( ImGuiKey_Backspace ) && len > 0 ) {
                buf[ --len ] = '\0'; changed = true;
            }
            for ( int i = 0; i < io.InputQueueCharacters.Size; i++ ) {
                const ImWchar c = io.InputQueueCharacters[ i ];
                if ( c < 32 || len >= buf_size - 1 ) continue;
                buf[ len++ ] = static_cast<char>( c );
                buf[ len ] = '\0'; changed = true;
            }
            if ( changed ) io.InputQueueCharacters.resize( 0 );
        }

        dl->AddRectFilled( pos, p1, IM_COL32( 10, 10, 18, 220 ), 4.0f );
        dl->AddRectFilledMultiColor( pos, ImVec2( p1.x, pos.y + h * 0.5f ),
            IM_COL32( 255, 255, 255, 6 ), IM_COL32( 255, 255, 255, 6 ), IM_COL32( 0, 0, 0, 0 ), IM_COL32( 0, 0, 0, 0 ) );

        const ImU32 bord = focused ? theme::accent_alpha( 180 ) : ( hov ? theme::accent_alpha( 80 ) : IM_COL32( 40, 40, 60, 150 ) );
        dl->AddRect( pos, p1, bord, 4.0f, 0, 1.5f );

        if ( focused ) {
            dl->AddRectFilled( ImVec2( pos.x - 2, pos.y - 2 ), ImVec2( p1.x + 2, p1.y + 2 ),
                IM_COL32( 107, 197, 255, 8 ), 6.0f );
        }

        const int len = static_cast<int>( strlen( buf ) );
        const float ty = pos.y + ( h - ImGui::GetTextLineHeight( ) ) * 0.5f;
        if ( len > 0 )
            dl->AddText( ImVec2( pos.x + 7.0f, ty ), theme::text_bright(), buf );
        else if ( !focused )
            dl->AddText( ImVec2( pos.x + 7.0f, ty ), theme::text_dim(), "..." );

        if ( focused && static_cast<int>( ImGui::GetTime( ) * 2.0 ) % 2 == 0 ) {
            const ImVec2 ts = ImGui::CalcTextSize( buf, buf + len );
            const float cx = pos.x + 7.0f + ts.x + 1.0f;
            dl->AddLine( ImVec2( cx, ty ), ImVec2( cx, ty + ImGui::GetTextLineHeight( ) ), theme::text_bright(), 1.0f );
        }

        return changed;
    }

    namespace _cp {
        inline ImGuiID open_id = 0;
        inline ImVec2 open_pos = {};
        inline ImVec4* editing_color = nullptr;
        inline float hue = 0.0f, sat = 1.0f, val = 1.0f, alpha = 1.0f;
        inline ImVec2 menu_pos_when_opened = {};
        inline bool dragging_inside = false;
    }

    inline bool color_picker( const char* label, ImVec4* color, float right_edge, float offset_from_right = 0.0f ) {
        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 cursor_pos = ImGui::GetCursorScreenPos( );
        const float picker_w = 20.0f, picker_h = 16.0f;
        const float pos_x = right_edge - picker_w - offset_from_right;
        const ImVec2 pos = ImVec2( pos_x, cursor_pos.y );

        ImGui::SetCursorScreenPos( pos );
        ImGui::InvisibleButton( label, ImVec2( picker_w, picker_h ) );
        const bool hov = ImGui::IsItemHovered( );
        const bool clicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );
        const ImGuiID my_id = ImGui::GetID( label );

        if ( clicked ) {
            if ( _cp::open_id == my_id ) _cp::open_id = 0;
            else {
                _cp::open_id = my_id;
                _cp::open_pos = ImVec2( pos.x + picker_w * 0.5f, pos.y + picker_h * 0.5f );
                _cp::editing_color = color;
                _cp::menu_pos_when_opened = ImGui::GetWindowPos( );
                ImGui::ColorConvertRGBtoHSV( color->x, color->y, color->z, _cp::hue, _cp::sat, _cp::val );
                _cp::alpha = color->w;
            }
        }

        const ImVec2 p1 = ImVec2( pos.x + picker_w, pos.y + picker_h );
        dl->AddRectFilled( pos, p1, IM_COL32( 10, 10, 18, 220 ), 3.0f );
        const ImU32 col_tl = ImGui::ColorConvertFloat4ToU32( *color );
        const ImVec4 darker = ImVec4( color->x * 0.5f, color->y * 0.5f, color->z * 0.5f, 1.0f );
        const ImU32 col_br = ImGui::ColorConvertFloat4ToU32( darker );
        dl->AddRectFilledMultiColor( ImVec2( pos.x + 1, pos.y + 1 ), ImVec2( p1.x - 1, p1.y - 1 ), col_tl, col_tl, col_br, col_br );

        if ( hov ) {
            dl->AddRect( pos, p1, theme::accent_alpha( 150 ), 3.0f, 0, 1.5f );
        }

        ImGui::SetCursorScreenPos( ImVec2( cursor_pos.x, cursor_pos.y ) );
        return false;
    }

    inline void render_open_color_picker( ) {
        if ( _cp::open_id == 0 || _cp::editing_color == nullptr ) { _cp::open_id = 0; return; }

        const ImVec2 current_menu_pos = ImGui::GetWindowPos( );
        if ( fabsf( current_menu_pos.x - _cp::menu_pos_when_opened.x ) > 0.1f || fabsf( current_menu_pos.y - _cp::menu_pos_when_opened.y ) > 0.1f ) {
            _cp::open_id = 0; return;
        }

        ImDrawList* fdl = ImGui::GetForegroundDrawList( );
        const float picker_w = 230.0f, picker_h = 260.0f;
        const float sv_size = 190.0f, hue_w = 16.0f, alpha_h = 16.0f, gap = 8.0f;
        const ImVec2 p0 = _cp::open_pos;
        const ImVec2 p1 = ImVec2( p0.x + picker_w, p0.y + picker_h );
        const ImVec2 mouse = ImGui::GetIO( ).MousePos;
        const bool mouse_down = ImGui::IsMouseDown( ImGuiMouseButton_Left );
        const bool mouse_clicked = ImGui::IsMouseClicked( ImGuiMouseButton_Left );
        const bool inside_picker = ( mouse.x >= p0.x && mouse.x <= p1.x && mouse.y >= p0.y && mouse.y <= p1.y );

        if ( inside_picker && mouse_clicked ) _cp::dragging_inside = true;
        if ( !mouse_down ) _cp::dragging_inside = false;
        if ( inside_picker || _cp::dragging_inside ) ImGui::GetIO( ).WantCaptureMouse = true;
        if ( mouse_clicked && !inside_picker ) { _cp::open_id = 0; _cp::dragging_inside = false; return; }

        fdl->AddRectFilled( ImVec2( p0.x + 2, p0.y + 2 ), ImVec2( p1.x + 2, p1.y + 2 ), IM_COL32( 0, 0, 0, 80 ), 6.0f );
        fdl->AddRectFilled( p0, p1, IM_COL32( 10, 10, 18, 220 ), 6.0f );
        fdl->AddRect( p0, p1, IM_COL32( 255, 255, 255, 18 ), 6.0f, 0, 1.5f );

        const ImVec2 sv_pos = ImVec2( p0.x + 12.0f, p0.y + 12.0f );
        const ImVec2 sv_end = ImVec2( sv_pos.x + sv_size, sv_pos.y + sv_size );

        ImVec4 hue_col;
        ImGui::ColorConvertHSVtoRGB( _cp::hue, 1.0f, 1.0f, hue_col.x, hue_col.y, hue_col.z );
        const ImU32 hue_u32 = ImGui::ColorConvertFloat4ToU32( ImVec4( hue_col.x, hue_col.y, hue_col.z, 1.0f ) );

        fdl->AddRectFilledMultiColor( sv_pos, sv_end, IM_COL32( 255, 255, 255, 255 ), hue_u32, hue_u32, IM_COL32( 0, 0, 0, 255 ) );
        fdl->AddRectFilledMultiColor( sv_pos, sv_end, IM_COL32( 0, 0, 0, 0 ), IM_COL32( 0, 0, 0, 0 ), IM_COL32( 0, 0, 0, 255 ), IM_COL32( 0, 0, 0, 255 ) );
        fdl->AddRect( sv_pos, sv_end, IM_COL32( 40, 40, 60, 100 ), 2.0f );

        const bool sv_hov = ( mouse.x >= sv_pos.x && mouse.x <= sv_end.x && mouse.y >= sv_pos.y && mouse.y <= sv_end.y );
        if ( sv_hov && mouse_down ) {
            _cp::sat = ImClamp( ( mouse.x - sv_pos.x ) / sv_size, 0.0f, 1.0f );
            _cp::val = 1.0f - ImClamp( ( mouse.y - sv_pos.y ) / sv_size, 0.0f, 1.0f );
            if ( _cp::editing_color ) {
                ImGui::ColorConvertHSVtoRGB( _cp::hue, _cp::sat, _cp::val, _cp::editing_color->x, _cp::editing_color->y, _cp::editing_color->z );
                _cp::editing_color->w = _cp::alpha;
            }
        }

        const float cursor_x = sv_pos.x + _cp::sat * sv_size;
        const float cursor_y = sv_pos.y + ( 1.0f - _cp::val ) * sv_size;
        fdl->AddCircleFilled( ImVec2( cursor_x, cursor_y ), 5.0f, IM_COL32( 255, 255, 255, 255 ), 12 );
        fdl->AddCircle( ImVec2( cursor_x, cursor_y ), 5.0f, IM_COL32( 0, 0, 0, 255 ), 12, 1.5f );

        const ImVec2 hue_pos = ImVec2( sv_end.x + gap, sv_pos.y );
        const ImVec2 hue_end = ImVec2( hue_pos.x + hue_w, sv_end.y );

        for ( int i = 0; i < 6; i++ ) {
            const float y0 = hue_pos.y + ( sv_size / 6.0f ) * static_cast<float>( i );
            const float y1 = hue_pos.y + ( sv_size / 6.0f ) * static_cast<float>( i + 1 );
            ImVec4 c0, c1;
            ImGui::ColorConvertHSVtoRGB( static_cast<float>( i ) / 6.0f, 1.0f, 1.0f, c0.x, c0.y, c0.z );
            ImGui::ColorConvertHSVtoRGB( static_cast<float>( i + 1 ) / 6.0f, 1.0f, 1.0f, c1.x, c1.y, c1.z );
            fdl->AddRectFilledMultiColor( ImVec2( hue_pos.x, y0 ), ImVec2( hue_end.x, y1 ),
                ImGui::ColorConvertFloat4ToU32( ImVec4( c0.x, c0.y, c0.z, 1.0f ) ),
                ImGui::ColorConvertFloat4ToU32( ImVec4( c0.x, c0.y, c0.z, 1.0f ) ),
                ImGui::ColorConvertFloat4ToU32( ImVec4( c1.x, c1.y, c1.z, 1.0f ) ),
                ImGui::ColorConvertFloat4ToU32( ImVec4( c1.x, c1.y, c1.z, 1.0f ) ) );
        }
        fdl->AddRect( hue_pos, hue_end, IM_COL32( 40, 40, 60, 100 ), 2.0f );

        const bool hue_hov = ( mouse.x >= hue_pos.x && mouse.x <= hue_end.x && mouse.y >= hue_pos.y && mouse.y <= hue_end.y );
        if ( hue_hov && mouse_down ) {
            _cp::hue = ImClamp( ( mouse.y - hue_pos.y ) / sv_size, 0.0f, 1.0f );
            if ( _cp::editing_color ) {
                ImGui::ColorConvertHSVtoRGB( _cp::hue, _cp::sat, _cp::val, _cp::editing_color->x, _cp::editing_color->y, _cp::editing_color->z );
                _cp::editing_color->w = _cp::alpha;
            }
        }

        const float hue_cursor_y = hue_pos.y + _cp::hue * sv_size;
        fdl->AddRectFilled( ImVec2( hue_pos.x - 2, hue_cursor_y - 2 ), ImVec2( hue_end.x + 2, hue_cursor_y + 2 ), IM_COL32( 255, 255, 255, 255 ), 1.0f );
        fdl->AddRect( ImVec2( hue_pos.x - 2, hue_cursor_y - 2 ), ImVec2( hue_end.x + 2, hue_cursor_y + 2 ), IM_COL32( 0, 0, 0, 255 ), 1.0f, 0, 1.5f );

        const ImVec2 alpha_pos = ImVec2( sv_pos.x, sv_end.y + gap );
        const ImVec2 alpha_end = ImVec2( sv_end.x, alpha_pos.y + alpha_h );

        ImVec4 current_rgb;
        ImGui::ColorConvertHSVtoRGB( _cp::hue, _cp::sat, _cp::val, current_rgb.x, current_rgb.y, current_rgb.z );
        const ImU32 col_opaque = ImGui::ColorConvertFloat4ToU32( ImVec4( current_rgb.x, current_rgb.y, current_rgb.z, 1.0f ) );
        fdl->AddRectFilledMultiColor( alpha_pos, alpha_end,
            IM_COL32( 0, 0, 0, 0 ), col_opaque, col_opaque, IM_COL32( 0, 0, 0, 0 ) );
        fdl->AddRect( alpha_pos, alpha_end, IM_COL32( 40, 40, 60, 100 ), 2.0f );

        const bool alpha_hov = ( mouse.x >= alpha_pos.x && mouse.x <= alpha_end.x && mouse.y >= alpha_pos.y && mouse.y <= alpha_end.y );
        if ( alpha_hov && mouse_down ) {
            _cp::alpha = ImClamp( ( mouse.x - alpha_pos.x ) / sv_size, 0.0f, 1.0f );
            if ( _cp::editing_color ) _cp::editing_color->w = _cp::alpha;
        }

        const float alpha_cursor_x = alpha_pos.x + _cp::alpha * sv_size;
        fdl->AddRectFilled( ImVec2( alpha_cursor_x - 2, alpha_pos.y - 2 ), ImVec2( alpha_cursor_x + 2, alpha_end.y + 2 ), IM_COL32( 255, 255, 255, 255 ), 1.0f );
        fdl->AddRect( ImVec2( alpha_cursor_x - 2, alpha_pos.y - 2 ), ImVec2( alpha_cursor_x + 2, alpha_end.y + 2 ), IM_COL32( 0, 0, 0, 255 ), 1.0f, 0, 1.5f );
    }

}