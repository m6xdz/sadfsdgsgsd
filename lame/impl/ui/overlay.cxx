#include <impl/ui/overlay.hxx>
#include <impl/ui/theme.hxx>
#include <impl/ui/animations.hxx>
#include <impl/ui/elements.hxx>
#include <impl/memory/input.hxx>
#include <impl/util/playfield.hxx>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <ShlObj.h>
#include <shobjidl.h>
#include <algorithm>
#include <random>
#include <cstring>

#pragma comment( lib, "d3d11.lib" )
#pragma comment( lib, "dxgi.lib" )
#pragma comment( lib, "d3dcompiler.lib" )
#pragma comment( lib, "dwmapi.lib" )
#pragma comment( lib, "shell32.lib" )
#pragma comment( lib, "ole32.lib" )
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam );

namespace {

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

    bool overlay_aim_hook_transform(
        void* ctx, POINT pen, const MSLLHOOKSTRUCT& raw, POINT* out ) {
        (void)pen;
        if ( !ctx || !out ) return false;
        auto* overlay = static_cast<ui::c_overlay*>( ctx );
        const auto adjusted = overlay->aim( ).apply_hook_move( pen, raw );
        if ( !adjusted ) return false;
        *out = *adjusted;
        return true;
    }

    bool overlay_keyboard_hook_callback( void* ctx, int vk, bool is_down, int64_t press_qpc ) {
        if ( !ctx ) return false;
        auto* overlay = static_cast<ui::c_overlay*>( ctx );
        return overlay->tap( ).handle_key_event( vk, is_down, press_qpc );
    }

    inline float& hover_anim( uint64_t key ) {
        static std::unordered_map<uint64_t, float> anims;
        return anims[ key ];
    }

    enum ACCENT_STATE {
        ACCENT_DISABLED = 0,
        ACCENT_ENABLE_GRADIENT = 1,
        ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
        ACCENT_ENABLE_BLURBEHIND = 3,
        ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
        ACCENT_ENABLE_HOSTBACKDROP = 5,
        ACCENT_INVALID_STATE = 6
    };

    struct ACCENT_POLICY {
        int State;
        int Flags;
        int GradientColor;
        int AnimationId;
    };

    struct WINCOMPATTRDATA {
        int Attribute;
        ACCENT_POLICY* Data;
        SIZE_T SizeOfData;
        int Reserved;
    };

    inline void enable_acrylic( HWND hwnd ) {
        HMODULE user32 = GetModuleHandleW( L"user32.dll" );
        if ( !user32 ) return;
        auto SetWindowCompositionAttribute = reinterpret_cast<BOOL (WINAPI*)( HWND, WINCOMPATTRDATA* )>(
            GetProcAddress( user32, "SetWindowCompositionAttribute" ) );
        if ( !SetWindowCompositionAttribute ) return;

        ACCENT_POLICY policy = { ACCENT_ENABLE_BLURBEHIND, 0, 0x00000000, 0 };
        WINCOMPATTRDATA data = { 19, &policy, sizeof( policy ), 0 };
        SetWindowCompositionAttribute( hwnd, &data );

        MARGINS margins = { -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea( hwnd, &margins );
    }

}

namespace ui {

    bool c_overlay::create( HINSTANCE instance ) {
        ImGui_ImplWin32_EnableDpiAwareness( );

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof( wc );
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = instance;
        wc.lpszClassName = L"  ";
        RegisterClassExW( &wc );

        m_hwnd = CreateWindowExW(
            WS_EX_APPWINDOW | WS_EX_LAYERED | WS_EX_TOPMOST,
            wc.lpszClassName, L"  ", WS_POPUP,
            0, 0, MENU_W, MENU_H,
            nullptr, nullptr, instance, this );

        if ( !m_hwnd ) return false;

        enable_acrylic( m_hwnd );
        SetLayeredWindowAttributes( m_hwnd, 0, 255, LWA_ALPHA );

        ShowWindow( m_hwnd, SW_SHOWDEFAULT );
        UpdateWindow( m_hwnd );

        if ( !init_d3d( ) ) return false;

        IMGUI_CHECKVERSION( );
        ImGui::CreateContext( );
        ImGuiIO& io = ImGui::GetIO( );
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImFontConfig font_cfg{};
        font_cfg.OversampleH = 3;
        font_cfg.OversampleV = 3;
        font_cfg.PixelSnapH = true;
        io.Fonts->AddFontFromFileTTF( "C:\\Windows\\Fonts\\segoeui.ttf", 16.f, &font_cfg );

        ImGuiStyle& style = ImGui::GetStyle( );
        style.WindowRounding = 12.f;
        style.WindowBorderSize = 0.f;

        ImGui_ImplWin32_Init( m_hwnd );
        ImGui_ImplDX11_Init( m_device, m_context );

        m_mouse_hook.set_filter_injected_only( true );
        m_mouse_hook.set_transform( overlay_aim_hook_transform, this );
        m_mouse_hook.install( );

        m_keyboard_hook.set_callback( overlay_keyboard_hook_callback, this );
        m_keyboard_hook.install( );

        m_aim.start();

        return true;
    }

    void c_overlay::destroy( ) {
        m_aim.stop( );
        m_mouse_hook.uninstall( );
        m_keyboard_hook.uninstall( );

        ImGui_ImplDX11_Shutdown( );
        ImGui_ImplWin32_Shutdown( );
        ImGui::DestroyContext( );
        cleanup_d3d( );
        if ( m_hwnd ) {
            DestroyWindow( m_hwnd );
            m_hwnd = nullptr;
        }
    }

    bool c_overlay::pump( ) {
        MSG msg;
        while ( PeekMessage( &msg, nullptr, 0, 0, PM_REMOVE ) ) {
            TranslateMessage( &msg );
            DispatchMessage( &msg );
            if ( msg.message == WM_QUIT )
                return false;
        }

        if ( stream_proof )
            SetWindowDisplayAffinity( m_hwnd, WDA_EXCLUDEFROMCAPTURE );
        else
            SetWindowDisplayAffinity( m_hwnd, WDA_NONE );

        handle_hotkeys( );
        update_overlay_position( );
        apply_visibility( );
        render_frame( );
        return true;
    }

    void c_overlay::handle_hotkeys( ) {
        const bool menu_down = ( GetAsyncKeyState( m_menu_keybind ) & 0x8000 ) != 0;
        if ( menu_down && !m_f4_was_down )
            m_visible = !m_visible;
        m_f4_was_down = menu_down;
    }

    void c_overlay::update_overlay_position( ) {
        if ( !m_hwnd ) return;

        const HWND osu_hwnd = input::target_window( );
        input::invalidate_virtual_desktop( );
        input::virtual_desktop( );

        int target_x = 0, target_y = 0, target_w = 0, target_h = 0;

        if ( !osu_hwnd || !IsWindow( osu_hwnd ) ) {
            target_w = GetSystemMetrics( SM_CXSCREEN );
            target_h = GetSystemMetrics( SM_CYSCREEN );
        }
        else {
            RECT client{};
            if ( playfield::get_playfield_rect( osu_hwnd, client ) ) {
                target_x = client.left;
                target_y = client.top;
                target_w = client.right - client.left;
                target_h = client.bottom - client.top;
            }
            else {
                target_w = GetSystemMetrics( SM_CXSCREEN );
                target_h = GetSystemMetrics( SM_CYSCREEN );
            }
        }

        SetWindowPos( m_hwnd, HWND_TOPMOST, target_x, target_y, target_w, target_h, SWP_NOACTIVATE );
    }

    void c_overlay::apply_visibility( ) {
        if ( !m_hwnd ) return;

        static bool prev_stream_proof = false;
        const bool stream_proof_changed = prev_stream_proof != stream_proof;
        prev_stream_proof = stream_proof;

        bool should_show = m_visible;

        if ( should_show ) {
            LONG ex = GetWindowLongW( m_hwnd, GWL_EXSTYLE );
            if ( stream_proof ) {
                ex |= WS_EX_TOOLWINDOW;
                ex &= ~WS_EX_APPWINDOW;
            } else {
                ex |= WS_EX_APPWINDOW;
                ex &= ~WS_EX_TOOLWINDOW;
            }
            if ( m_visible ) {
                ex &= ~WS_EX_TRANSPARENT;
            } else {
                ex |= WS_EX_TRANSPARENT;
            }
            SetWindowLongW( m_hwnd, GWL_EXSTYLE, ex );
            ShowWindow( m_hwnd, SW_SHOWNA );

            if ( stream_proof_changed ) {
                ShowWindow( m_hwnd, SW_HIDE );
                ShowWindow( m_hwnd, SW_SHOWNA );
            }
        }
        else {
            ShowWindow( m_hwnd, SW_HIDE );
        }
    }

    LRESULT CALLBACK c_overlay::wnd_proc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp ) {
        if ( ImGui_ImplWin32_WndProcHandler( hwnd, msg, wp, lp ) )
            return true;

        if ( msg == WM_NCCREATE ) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>( lp );
            SetWindowLongPtrW( hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>( cs->lpCreateParams ) );
        }

        auto* self = reinterpret_cast<c_overlay*>( GetWindowLongPtrW( hwnd, GWLP_USERDATA ) );
        if ( msg == WM_DESTROY ) {
            PostQuitMessage( 0 );
            return 0;
        }
        if ( msg == WM_SETCURSOR ) {
            if ( self && self->stream_proof && self->m_streamproof_hide_cursor ) {
                SetCursor( nullptr );
                return 1;
            }
            SetCursor( LoadCursorW( nullptr, IDC_ARROW ) );
            return 1;
        }
        if ( msg == WM_SIZE && self && self->m_swap_chain ) {
            UINT w = LOWORD( lp ), h = HIWORD( lp );
            if ( w == 0 || h == 0 ) return 0;
            if ( self->m_rtv ) {
                self->m_rtv->Release( );
                self->m_rtv = nullptr;
            }
            self->m_swap_chain->ResizeBuffers( 0, w, h, DXGI_FORMAT_UNKNOWN, 0 );
            ID3D11Texture2D* back = nullptr;
            if ( SUCCEEDED( self->m_swap_chain->GetBuffer( 0, IID_PPV_ARGS( &back ) ) ) && back ) {
                self->m_device->CreateRenderTargetView( back, nullptr, &self->m_rtv );
                back->Release( );
            }
        }
        return DefWindowProcW( hwnd, msg, wp, lp );
    }

    bool c_overlay::init_d3d( ) {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = m_hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        D3D_FEATURE_LEVEL level{};
        if ( D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &sd, &m_swap_chain, &m_device, &level, &m_context ) != S_OK )
            return false;

        ID3D11Texture2D* back = nullptr;
        m_swap_chain->GetBuffer( 0, IID_PPV_ARGS( &back ) );
        if ( !back ) return false;
        m_device->CreateRenderTargetView( back, nullptr, &m_rtv );
        back->Release( );
        return m_rtv != nullptr;
    }

    void c_overlay::cleanup_d3d( ) {
        if ( m_rtv ) { m_rtv->Release( ); m_rtv = nullptr; }
        if ( m_swap_chain ) { m_swap_chain->Release( ); m_swap_chain = nullptr; }
        if ( m_context ) { m_context->Release( ); m_context = nullptr; }
        if ( m_device ) { m_device->Release( ); m_device = nullptr; }
    }

    void c_overlay::render_frame( ) {
        osu::full_snapshot_t snap;
        if ( m_snapshot_fn )
            snap = m_snapshot_fn( );

        bool should_show = m_visible;
        if ( !should_show ) {
            if ( m_context && m_rtv ) {
                const float clear[ 4 ]{ 0.0f, 0.0f, 0.0f, 0.0f };
                m_context->OMSetRenderTargets( 1, &m_rtv, nullptr );
                m_context->ClearRenderTargetView( m_rtv, clear );
            }
            if ( m_swap_chain ) m_swap_chain->Present( 0, 0 );
            return;
        }

        ImGui_ImplDX11_NewFrame( );
        ImGui_ImplWin32_NewFrame( );
        ImGui::NewFrame( );

        const float dt = ImGui::GetIO( ).DeltaTime;
        tick_animations( dt );
        m_anim_time = g_time;

        if ( m_visible && !m_menu_was_open ) {
            m_menu_open_anim = 0.f;
            m_menu_was_open = true;
        }
        else if ( !m_visible && m_menu_was_open ) {
            m_menu_was_open = false;
        }

        if ( m_visible ) {
            m_menu_open_anim += ( 1.0f - m_menu_open_anim ) * std::min( dt * 6.0f, 1.0f );
        }

        if ( m_visible ) {
            ImDrawList* bg_dl = ImGui::GetBackgroundDrawList( );
            const ImVec2 disp = ImGui::GetIO( ).DisplaySize;
            const float bg_a = m_menu_open_anim * 0.2f;
            if ( bg_a > 0.01f ) {
                const int a = static_cast<int>( bg_a * 255 );
                bg_dl->AddRectFilledMultiColor(
                    ImVec2( 0, 0 ), disp,
                    IM_COL32( 2, 2, 12, a ), IM_COL32( 2, 2, 12, a ),
                    IM_COL32( 4, 2, 16, a ), IM_COL32( 4, 2, 16, a ) );
            }
        }

        if ( m_visible ) {
            draw_menu( snap );
            m_streamproof_hide_cursor = stream_proof && ( ImGui::GetIO( ).WantCaptureMouse );
        }
        else {
            m_streamproof_hide_cursor = false;
        }

        ImGui::Render( );

        if ( m_context && m_rtv ) {
            const float clear_color[ 4 ]{ 0.0f, 0.0f, 0.0f, 0.0f };
            m_context->OMSetRenderTargets( 1, &m_rtv, nullptr );
            m_context->ClearRenderTargetView( m_rtv, clear_color );
        }

        ImGui_ImplDX11_RenderDrawData( ImGui::GetDrawData( ) );
        if ( m_swap_chain ) m_swap_chain->Present( 0, 0 );
    }

    void c_overlay::apply_custom_keys( osu::game_snapshot_t& game ) const {
        game.left_key = m_custom_left_key;
        game.right_key = m_custom_right_key;
    }

    config::settings_t c_overlay::capture_settings( ) const {
        config::settings_t s{};
        s.aim_enabled = m_aim.enabled;
        s.aim_ignore_sliders = m_aim.ignore_sliders;
        s.aim_tablet_mode = m_aim.tablet_mode;

        s.aim_strength_x = m_aim.strength_x;
        s.aim_strength_y = m_aim.strength_y;
        s.aim_decay_far = m_aim.decay_far;
        s.aim_lerp = m_aim.aim_lerp;
        s.aim_freeze_lerp = m_aim.freeze_lerp;
        s.aim_window = m_aim.aim_window;
        s.aim_legit_mode = m_aim.legit_mode;
        s.aim_legit_clamp = m_aim.legit_clamp;

        s.relax_enabled = m_relax.enabled;
        s.relax_ur = m_relax.ur;
        s.relax_tap_style = m_relax.tap_style;
        s.relax_singletap_bpm_cap = m_relax.singletap_bpm_cap;
        s.relax_k1_hold_center = m_relax.k1_hold_center;
        s.relax_k1_hold_spread = m_relax.k1_hold_spread;
        s.relax_k2_hold_center = m_relax.k2_hold_center;
        s.relax_k2_hold_spread = m_relax.k2_hold_spread;
        s.relax_hold_floor = m_relax.hold_floor;
        s.relax_hold_ceiling = m_relax.hold_ceiling;
        s.relax_manual_offset_ms = m_relax.manual_offset_ms;

        s.replay_enabled = m_replay.enabled;
        s.replay_path_utf8 = m_replay_path_utf8;
        s.replay_parse_buttons = m_replay.parse_buttons;

        s.autobot_enabled = m_autobot.enabled;
        s.autobot_aim_spread = m_autobot.aim_spread;
        s.autobot_curve_strength = m_autobot.curve_strength;
        s.autobot_drift_amount = m_autobot.drift_amount;
        s.autobot_momentum = m_autobot.momentum;
        s.autobot_slider_laziness = m_autobot.slider_laziness;
        s.autobot_spinner_rpm = m_autobot.spinner_rpm;

        s.tap_enabled = m_tap_assist.enabled;
        s.tap_assist_window = m_tap_assist.assist_window;
        s.tap_randomization = m_tap_assist.randomization;
        s.tap_ignore_sliders = m_tap_assist.ignore_sliders;

        s.custom_left_key = m_custom_left_key;
        s.custom_right_key = m_custom_right_key;
        s.menu_keybind = m_menu_keybind;
        s.stream_proof = stream_proof;
        s.songs_path_utf8 = m_songs_path_utf8;

        return s;
    }

    void c_overlay::apply_settings( const config::settings_t& s ) {
        m_aim.enabled = s.aim_enabled;
        m_aim.ignore_sliders = s.aim_ignore_sliders;
        m_aim.tablet_mode = s.aim_tablet_mode;

        m_aim.strength_x = s.aim_strength_x;
        m_aim.strength_y = s.aim_strength_y;
        m_aim.decay_far = s.aim_decay_far;
        m_aim.aim_lerp = s.aim_lerp;
        m_aim.freeze_lerp = s.aim_freeze_lerp;
        m_aim.aim_window = s.aim_window;
        m_aim.legit_mode = s.aim_legit_mode;
        m_aim.legit_clamp = s.aim_legit_clamp;

        m_relax.enabled = s.relax_enabled;
        m_relax.ur = s.relax_ur;
        m_relax.tap_style = s.relax_tap_style;
        m_relax.singletap_bpm_cap = s.relax_singletap_bpm_cap;
        m_relax.k1_hold_center = s.relax_k1_hold_center;
        m_relax.k1_hold_spread = s.relax_k1_hold_spread;
        m_relax.k2_hold_center = s.relax_k2_hold_center;
        m_relax.k2_hold_spread = s.relax_k2_hold_spread;
        m_relax.hold_floor = s.relax_hold_floor;
        m_relax.hold_ceiling = s.relax_hold_ceiling;
        m_relax.manual_offset_ms = s.relax_manual_offset_ms;

        m_replay.enabled = s.replay_enabled;
        if ( !s.replay_path_utf8.empty( ) ) {
            strncpy_s( m_replay_path_utf8, s.replay_path_utf8.c_str( ), _TRUNCATE );
            int wlen = MultiByteToWideChar( CP_UTF8, 0, m_replay_path_utf8, -1, nullptr, 0 );
            if ( wlen > 1 ) {
                std::wstring wide( static_cast<size_t>( wlen - 1 ), L'\0' );
                MultiByteToWideChar( CP_UTF8, 0, m_replay_path_utf8, -1, wide.data( ), wlen );
                m_replay.replay_path = wide;
                m_replay.load_replay( );
                m_replay.reset_sync( );
            }
        }

        m_replay.parse_buttons = s.replay_parse_buttons;

        m_autobot.enabled = s.autobot_enabled;
        m_autobot.aim_spread = s.autobot_aim_spread;
        m_autobot.curve_strength = s.autobot_curve_strength;
        m_autobot.drift_amount = s.autobot_drift_amount;
        m_autobot.momentum = s.autobot_momentum;
        m_autobot.slider_laziness = s.autobot_slider_laziness;
        m_autobot.spinner_rpm = s.autobot_spinner_rpm;

        m_tap_assist.enabled = s.tap_enabled;
        m_tap_assist.assist_window = s.tap_assist_window;
        m_tap_assist.randomization = s.tap_randomization;
        m_tap_assist.ignore_sliders = s.tap_ignore_sliders;

        m_custom_left_key = s.custom_left_key;
        m_custom_right_key = s.custom_right_key;
        m_menu_keybind = s.menu_keybind;
        stream_proof = s.stream_proof;



        if ( !s.songs_path_utf8.empty( ) ) {
            strncpy_s( m_songs_path_utf8, s.songs_path_utf8.c_str( ), _TRUNCATE );
            if ( m_cache ) {
                wchar_t wide[ 512 ]{};
                MultiByteToWideChar( CP_UTF8, 0, m_songs_path_utf8, -1, wide, 512 );
                m_cache->stable_parser( ).set_songs_path( wide );
                m_cache->invalidate_beatmap_cache( );
            }
        }
    }

    void c_overlay::reset_modules( const osu::game_snapshot_t& game ) {
        m_game_time_stall_start_ms = 0;
        m_aim.set_user_input_blocked( false );
        m_aim.on_leave_play( );
        osu::game_snapshot_t mod_game = game;
        apply_custom_keys( mod_game );
        m_relax.on_leave_play( mod_game );
        m_replay.on_leave_play( mod_game );
        m_autobot.on_leave_play( mod_game );
        m_tap_assist.on_leave_play( mod_game );
    }

    void c_overlay::tick_modules( const osu::game_snapshot_t& game, const osu::beatmap_data_t& beatmap ) {
        const bool in_play = game.cur_state == osu::game_state_t::play;
        const bool replay_active = game.is_replay;
        const bool was_play = m_prev_state == osu::game_state_t::play;
        const std::string map_sig =
            std::to_string( game.map_id ) + "|" + std::to_string( game.set_id ) + "|" +
            game.map_folder + "|" + game.map_file + "|" + game.beatmap_hash + "|" +
            game.beatmap_version;

        if ( replay_active ) {
            m_prev_game_time = -1;
            m_prev_map_id = -1;
            m_prev_map_sig.clear( );
            m_game_time_stall_start_ms = 0;
            m_aim.set_user_input_blocked( true );
            return;
        }

        if ( was_play && !in_play ) {
            reset_modules( game );
            m_prev_game_time = -1;
            m_prev_map_id = -1;
            m_prev_map_sig.clear( );
            m_game_time_stall_start_ms = 0;
        }

        if ( in_play && !map_sig.empty( ) && !m_prev_map_sig.empty( ) && map_sig != m_prev_map_sig )
            reset_modules( game );

        if ( in_play && m_prev_map_id > 0 && game.map_id != 0 && game.map_id != m_prev_map_id )
            reset_modules( game );

        if ( was_play && in_play && m_prev_game_time >= 0 && game.cur_time < m_prev_game_time - 200 )
            reset_modules( game );

        m_prev_state = game.cur_state;

        if ( in_play && beatmap.loaded && !beatmap.objects.empty( ) ) {
            osu::game_snapshot_t mod_game = game;
            apply_custom_keys( mod_game );

            constexpr uint64_t k_pause_stall_ms = 120;
            const uint64_t     now_ms = get_time_ms( );
            const int32_t      cur_time = mod_game.cur_time;
            const bool         time_stalled = m_prev_game_time >= 0 && cur_time == m_prev_game_time;

            if ( time_stalled ) {
                if ( m_game_time_stall_start_ms == 0 )
                    m_game_time_stall_start_ms = now_ms;
            }
            else {
                m_game_time_stall_start_ms = 0;
            }

            const bool map_paused = m_game_time_stall_start_ms != 0
                                    && ( now_ms - m_game_time_stall_start_ms ) >= k_pause_stall_ms;

            m_aim.set_user_input_blocked( map_paused );

            if ( !map_paused )
                m_aim.update( mod_game, beatmap );

            m_relax.update( mod_game, beatmap );
            m_replay.update( mod_game, beatmap, map_paused );
            m_autobot.update( mod_game, beatmap, map_paused );
            m_tap_assist.update( mod_game, beatmap );
        }
        else {
            m_game_time_stall_start_ms = 0;
            m_aim.set_user_input_blocked( false );
        }

        if ( in_play ) {
            m_prev_map_id = game.map_id;
            m_prev_game_time = game.cur_time;
            if ( !map_sig.empty( ) )
                m_prev_map_sig = map_sig;
        }
    }

    void c_overlay::draw_menu( const osu::full_snapshot_t& snap ) {
        update_bind_capture( );

        ImGuiIO& io = ImGui::GetIO( );

        static const float MENU_W = 900.f;
        static const float MENU_H = 620.f;
        static const float SIDEBAR_W = 180.f;
        static const float TITLE_H = 34.f;
        static const float PADDING = 18.f;
        static const float L_X = SIDEBAR_W + PADDING;
        static const float L_W = 330.f;
        static const float GAP = 14.f;
        static const float R_X = L_X + L_W + GAP;
        static const float R_W = 330.f;

        const float scale = theme::ease_out_back( std::min( m_menu_open_anim * 1.2f, 1.0f ) );
        const float alpha = std::min( m_menu_open_anim * 1.5f, 1.0f );

        float menu_x = ( io.DisplaySize.x - MENU_W * scale ) * 0.5f + m_menu_offset_x;
        float menu_y = ( io.DisplaySize.y - MENU_H * scale ) * 0.5f + m_menu_offset_y +
                       ( 1.0f - scale ) * 30.0f;

        ImGui::SetNextWindowPos( ImVec2( menu_x, menu_y ), ImGuiCond_Always );
        ImGui::SetNextWindowSize( ImVec2( MENU_W * scale, MENU_H * scale ), ImGuiCond_Always );

        ImGuiWindowFlags wf =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoMove;

        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 0 ) );
        ImGui::Begin( "##  ", nullptr, wf );
        ImGui::PopStyleVar( 2 );

        ImDrawList* dl = ImGui::GetWindowDrawList( );
        const ImVec2 wpos = ImGui::GetWindowPos( );
        const ImVec2 wsize = ImGui::GetWindowSize( );
        auto S = [&]( float x, float y ) { return ImVec2( wpos.x + x, wpos.y + y ); };

        if ( alpha > 0.01f ) {
            const ImVec2 br = ImVec2( wpos.x + wsize.x, wpos.y + wsize.y );

            dl->AddRectFilled( ImVec2( wpos.x + 4, wpos.y + 4 ), ImVec2( br.x + 4, br.y + 4 ),
                IM_COL32( 0, 0, 0, static_cast<int>( 60 * alpha ) ), 12.0f );

            dl->AddRectFilled( wpos, br, IM_COL32( 12, 12, 20, static_cast<int>( 180 * alpha ) ), 12.0f );

            dl->AddRectFilledMultiColor(
                wpos, ImVec2( br.x, wpos.y + wsize.y * 0.15f ),
                IM_COL32( 255, 255, 255, static_cast<int>( 8 * alpha ) ),
                IM_COL32( 255, 255, 255, static_cast<int>( 8 * alpha ) ),
                IM_COL32( 255, 255, 255, 0 ),
                IM_COL32( 255, 255, 255, 0 ) );

            dl->AddRect( wpos, br, IM_COL32( 255, 255, 255, static_cast<int>( 20 * alpha ) ), 12.0f, 0, 1.0f );

            const float accent_breathe = breathe( 1.2f, 0.3f, 1.0f );
            const int accent_a = static_cast<int>( 50 * alpha * accent_breathe );
            dl->AddRectFilled( ImVec2( wpos.x + 20, br.y - 2 ), ImVec2( br.x - 20, br.y ),
                IM_COL32( 107, 197, 255, accent_a ), 1.0f );
        }

        {
            const ImVec2 sb_br = ImVec2( wpos.x + SIDEBAR_W, wpos.y + wsize.y );
            dl->AddRectFilled( wpos, sb_br, IM_COL32( 10, 10, 18, static_cast<int>( 220 * alpha ) ), 12.0f, ImDrawFlags_RoundCornersLeft );

            const float div_breathe = breathe( 1.5f, 8.f, 20.f );
            dl->AddLine( ImVec2( wpos.x + SIDEBAR_W, wpos.y ), ImVec2( wpos.x + SIDEBAR_W, wpos.y + wsize.y ),
                IM_COL32( 107, 197, 255, static_cast<int>( div_breathe * alpha ) ), 1.0f );
        }

        {
            const float logo_anim = std::min( m_menu_open_anim * 3.0f, 1.0f );
            const char* logo_text = "LAME";
            const ImVec2 logo_size = ImGui::CalcTextSize( logo_text );
            const float lx = wpos.x + ( SIDEBAR_W - logo_size.x ) * 0.5f;
            const float ly = wpos.y + 24.0f + ( 1.0f - logo_anim ) * 10.0f;

            dl->AddText( ImVec2( lx, ly ), IM_COL32( 107, 197, 255, static_cast<int>( 240 * logo_anim ) ), logo_text );

            const char* sub_text = "osu! cheetos";
            const ImVec2 sub_size = ImGui::CalcTextSize( sub_text );
            const float sx = wpos.x + ( SIDEBAR_W - sub_size.x ) * 0.5f;
            dl->AddText( ImVec2( sx, ly + 20.0f ),
                IM_COL32( 150, 170, 200, static_cast<int>( 180 * logo_anim ) ), sub_text );
        }

        {
            update_tab_transition( m_tab, io.DeltaTime );

            static const char* tab_names[ ] = { "Aimbot", "Relax", "Tap Assist", "Replay", "Autobot", "System", "Config" };
            const float tab_start_y = wpos.y + 76.0f;
            const float tab_h = 36.0f;
            const float tab_gap = 3.0f;

            const ImVec2 mouse = ImGui::GetIO( ).MousePos;
            const bool mouse_clicked = ImGui::IsMouseClicked( ImGuiMouseButton_Left );

            float target_indicator_y = tab_start_y + static_cast<float>( m_tab ) * ( tab_h + tab_gap );
            g_tab_indicator_y += ( target_indicator_y - g_tab_indicator_y ) * std::min( io.DeltaTime * 14.0f, 1.0f );

            for ( int i = 0; i < 7; i++ ) {
                const float tx = wpos.x + 8.0f;
                const float ty = tab_start_y + static_cast<float>( i ) * ( tab_h + tab_gap );
                const float tw = SIDEBAR_W - 16.0f;

                const ImVec2 btn_min = ImVec2( tx, ty );
                const ImVec2 btn_max = ImVec2( tx + tw, ty + tab_h );

                const bool hov = ( mouse.x >= btn_min.x && mouse.x <= btn_max.x && mouse.y >= btn_min.y && mouse.y <= btn_max.y );
                const bool active = ( i == m_tab );

                if ( hov && mouse_clicked ) {
                    m_tab = i;
                }

                float& anim = tab_hover_anim( i );
                const float target = ( hov || active ) ? 1.0f : 0.0f;
                anim += ( target - anim ) * std::min( io.DeltaTime * 12.0f, 1.0f );

                if ( active ) {
                    const float breath = breathe( 2.0f, 0.7f, 1.0f );
                    const int ind_a = static_cast<int>( 220 * alpha * breath );
                    const float ind_y = g_tab_indicator_y;
                    dl->AddRectFilled( ImVec2( btn_min.x, ind_y + 3 ), ImVec2( btn_min.x + 3, ind_y + tab_h - 3 ),
                        IM_COL32( 107, 197, 255, ind_a ), 1.5f );
                }

                if ( anim > 0.01f ) {
                    const int a = static_cast<int>( anim * 30 * alpha );
                    dl->AddRectFilled( btn_min, btn_max, IM_COL32( 107, 197, 255, a ), 8.0f );
                }

                const float ic_x = btn_min.x + 14.0f;
                const float ic_y = btn_min.y + tab_h * 0.5f;
                const ImU32 ic_col = active ? theme::accent() : ( hov ? theme::text_bright() : IM_COL32( 90, 100, 120, 200 ) );
                const ImU32 ic_col_dim = active ? theme::accent_alpha( 120 ) : IM_COL32( 90, 100, 120, 70 );
                const float icon_scale = active ? ( 1.0f + breathe( 1.8f, 0.f, 0.08f ) ) : 1.0f;

                switch ( i ) {
                    case 0: {
                        dl->AddCircle( ImVec2( ic_x, ic_y ), 6.0f * icon_scale, ic_col, 20, 1.5f );
                        dl->AddCircleFilled( ImVec2( ic_x, ic_y ), 1.8f * icon_scale, ic_col, 12 );
                        dl->AddLine( ImVec2( ic_x - 8.f, ic_y ), ImVec2( ic_x - 3.f, ic_y ), ic_col, 1.3f );
                        dl->AddLine( ImVec2( ic_x + 3.f, ic_y ), ImVec2( ic_x + 8.f, ic_y ), ic_col, 1.3f );
                        dl->AddLine( ImVec2( ic_x, ic_y - 8.f ), ImVec2( ic_x, ic_y - 3.f ), ic_col, 1.3f );
                        dl->AddLine( ImVec2( ic_x, ic_y + 3.f ), ImVec2( ic_x, ic_y + 8.f ), ic_col, 1.3f );
                        break;
                    }
                    case 1: {
                        const float ww = 12.f;
                        dl->AddBezierCubic(
                            ImVec2( ic_x - ww * 0.5f, ic_y + 3.f ),
                            ImVec2( ic_x - ww * 0.15f, ic_y - 5.f ),
                            ImVec2( ic_x + ww * 0.15f, ic_y + 5.f ),
                            ImVec2( ic_x + ww * 0.5f, ic_y - 3.f ),
                            ic_col, 1.8f, 12 );
                        dl->AddBezierCubic(
                            ImVec2( ic_x - ww * 0.5f, ic_y + 3.f ),
                            ImVec2( ic_x - ww * 0.15f, ic_y - 5.f ),
                            ImVec2( ic_x + ww * 0.15f, ic_y + 5.f ),
                            ImVec2( ic_x + ww * 0.5f, ic_y - 3.f ),
                            ic_col_dim, 4.f, 12 );
                        break;
                    }
                    case 2: {
                        const float pulse2 = active ? breathe( 2.5f, 0.f, 1.5f ) : 0.f;
                        dl->AddCircle( ImVec2( ic_x - 3.f, ic_y ), 3.5f + pulse2, ic_col, 12, 1.6f );
                        dl->AddCircleFilled( ImVec2( ic_x - 3.f, ic_y ), 1.5f, ic_col, 8 );
                        dl->AddCircle( ImVec2( ic_x + 3.f, ic_y ), 3.5f + pulse2 * 0.7f, ic_col, 12, 1.6f );
                        dl->AddCircleFilled( ImVec2( ic_x + 3.f, ic_y ), 1.5f, ic_col, 8 );
                        break;
                    }
                    case 3: {
                        const float arc_r = 5.5f;
                        const int segs = 12;
                        for ( int s = 0; s < segs; ++s ) {
                            const float a1 = -2.8f + ( 6.3f ) * s / segs;
                            const float a2 = -2.8f + ( 6.3f ) * ( s + 1 ) / segs;
                            dl->AddLine(
                                ImVec2( ic_x + arc_r * std::cos( a1 ), ic_y + arc_r * std::sin( a1 ) ),
                                ImVec2( ic_x + arc_r * std::cos( a2 ), ic_y + arc_r * std::sin( a2 ) ),
                                ic_col, 1.8f );
                        }
                        const float tip_a = 2.2f;
                        const float tip_r = 5.5f;
                        const float tip_x = ic_x + tip_r * std::cos( tip_a );
                        const float tip_y = ic_y + tip_r * std::sin( tip_a );
                        dl->AddTriangleFilled(
                            ImVec2( tip_x + 2.5f, tip_y - 3.f ),
                            ImVec2( tip_x + 2.5f, tip_y + 1.f ),
                            ImVec2( tip_x - 1.f, tip_y - 1.f ),
                            ic_col );
                        break;
                    }
                    case 4: {
                        const float gr = 6.5f;
                        for ( int s = 0; s < 8; ++s ) {
                            const float a1 = 6.2832f / 8.f * s - 1.5708f;
                            const float a2 = 6.2832f / 8.f * ( s + 1 ) - 1.5708f;
                            const float inner_r = ( s % 2 == 0 ) ? gr - 1.5f : gr;
                            dl->AddLine(
                                ImVec2( ic_x + inner_r * std::cos( a1 ), ic_y + inner_r * std::sin( a1 ) ),
                                ImVec2( ic_x + gr * std::cos( ( a1 + a2 ) * 0.5f ), ic_y + gr * std::sin( ( a1 + a2 ) * 0.5f ) ),
                                ic_col, 1.8f );
                            dl->AddLine(
                                ImVec2( ic_x + gr * std::cos( ( a1 + a2 ) * 0.5f ), ic_y + gr * std::sin( ( a1 + a2 ) * 0.5f ) ),
                                ImVec2( ic_x + inner_r * std::cos( a2 ), ic_y + inner_r * std::sin( a2 ) ),
                                ic_col, 1.8f );
                        }
                        dl->AddCircleFilled( ImVec2( ic_x, ic_y ), 2.0f, ic_col, 8 );
                        break;
                    }
                    case 5: {
                        const float bw = 10.f, bh = 2.f;
                        dl->AddRectFilled( ImVec2( ic_x - bw * 0.5f, ic_y - 5.f ), ImVec2( ic_x + bw * 0.5f, ic_y - 5.f + bh ), ic_col, 1.f );
                        dl->AddRectFilled( ImVec2( ic_x - bw * 0.5f, ic_y - 1.f ), ImVec2( ic_x + bw * 0.5f, ic_y - 1.f + bh ), ic_col, 1.f );
                        dl->AddRectFilled( ImVec2( ic_x - bw * 0.5f, ic_y + 3.f ), ImVec2( ic_x + bw * 0.5f, ic_y + 3.f + bh ), ic_col, 1.f );
                        dl->AddCircleFilled( ImVec2( ic_x + bw * 0.5f + 1.f, ic_y - 4.f ), 1.2f, ic_col, 6 );
                        dl->AddCircleFilled( ImVec2( ic_x + bw * 0.5f + 1.f, ic_y + 0.f ), 1.2f, ic_col, 6 );
                        dl->AddCircleFilled( ImVec2( ic_x + bw * 0.5f + 1.f, ic_y + 4.f ), 1.2f, ic_col, 6 );
                        break;
                    }
                    case 6: {
                        const float cx = ic_x;
                        const float cy = ic_y;
                        dl->AddRectFilled( ImVec2( cx - 5.f, cy - 2.f ), ImVec2( cx + 5.f, cy + 4.f ), ic_col, 1.5f );
                        dl->AddCircleFilled( ImVec2( cx - 3.f, cy - 1.f ), 3.2f, ic_col, 10 );
                        dl->AddCircleFilled( ImVec2( cx, cy - 3.f ), 3.8f, ic_col, 10 );
                        dl->AddCircleFilled( ImVec2( cx + 3.f, cy - 1.f ), 3.2f, ic_col, 10 );
                        break;
                    }
                }

                const ImVec2 ts = ImGui::CalcTextSize( tab_names[ i ] );
                const float label_x = btn_min.x + 30.0f;
                const float label_y = btn_min.y + ( tab_h - ts.y ) * 0.5f;
                const ImU32 text_col = active ? theme::text_accent() : ( hov ? theme::text_bright() : theme::text() );
                dl->AddText( ImVec2( label_x, label_y ), text_col, tab_names[ i ] );
            }
        }

        {
            const float title_y = wpos.y;
            const float title_h = TITLE_H;
            const ImVec2 tp0 = ImVec2( wpos.x + SIDEBAR_W + 1, title_y );
            const ImVec2 tp1 = ImVec2( wpos.x + wsize.x, title_y + title_h );

            dl->AddRectFilled( tp0, tp1, IM_COL32( 10, 10, 18, static_cast<int>( 200 * alpha ) ), 12.0f, ImDrawFlags_RoundCornersTopRight );

            ImGui::SetCursorPos( ImVec2( SIDEBAR_W + 1, 0 ) );
            ImGui::InvisibleButton( "##titlebar", ImVec2( wsize.x - SIDEBAR_W - 35.0f, title_h ) );
            if ( ImGui::IsItemActive( ) ) {
                m_menu_offset_x += static_cast<int>( ImGui::GetIO( ).MouseDelta.x );
                m_menu_offset_y += static_cast<int>( ImGui::GetIO( ).MouseDelta.y );
            }
        }

        {
            const float CLOSE_W = 20.0f;
            ImGui::SetCursorPos( ImVec2( wsize.x - CLOSE_W - 12.0f, 0.0f ) );
            ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0, 0, 0, 0 ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0, 0, 0, 0 ) );
            ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0, 0, 0, 0 ) );
            ImGui::PushStyleColor( ImGuiCol_Text, theme::text_dim() );
            if ( ImGui::Button( "X##close", ImVec2( CLOSE_W, TITLE_H ) ) ) {
                PostMessageW( m_hwnd, WM_CLOSE, 0, 0 );
            }
            ImGui::PopStyleColor( 4 );
        }

        const float entrance = card_entrance( 0, 0.0f );
        const float content_alpha = entrance;
        const float content_slide = ( 1.0f - entrance ) * 12.0f;

        if ( snap.game.is_replay ) {
            const float warn_y = wpos.y + TITLE_H + 10.0f;
            dl->AddRectFilled(
                ImVec2( wpos.x + SIDEBAR_W + 12.0f, warn_y ),
                ImVec2( wpos.x + wsize.x - 12.0f, warn_y + 30.0f ),
                IM_COL32( 200, 50, 50, static_cast<int>( 60 * alpha ) ), 6.0f );
            dl->AddText(
                ImVec2( wpos.x + SIDEBAR_W + 22.0f, warn_y + 7.0f ),
                IM_COL32( 255, 100, 100, static_cast<int>( 255 * alpha ) ), "RAD" );
        }

        const float replay_banner_h = snap.game.is_replay ? 36.0f : 0.0f;

        if ( m_tab == 0 ) {
            float lbox_top = TITLE_H + 14.0f + content_slide + replay_banner_h;
            float ly = lbox_top + 30.0f;
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            ImGui::SetCursorPos(ImVec2(L_X + 12.0f, ly));
            checkbox("Enable aim assist", &m_aim.enabled);
            ly = ImGui::GetCursorPos().y + 4.0f;
            ImGui::SetCursorPos(ImVec2(L_X + 12.0f, ly));
            checkbox("Ignore sliders", &m_aim.ignore_sliders);
            ly = ImGui::GetCursorPos().y + 4.0f;
            ImGui::SetCursorPos(ImVec2(L_X + 12.0f, ly));
            checkbox("Tablet mode", &m_aim.tablet_mode);
            ly = ImGui::GetCursorPos().y + 4.0f;
            ImGui::SetCursorPos(ImVec2(L_X + 12.0f, ly));
            checkbox("Legit mode", &m_aim.legit_mode);
            ly = ImGui::GetCursorPos().y + 4.0f;
            const float lbox_bottom = ly + 12.0f;
            dl->ChannelsSetCurrent(0);
            draw_glass_card( dl, S(L_X, lbox_top), S(L_X + L_W, lbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( L_X + 14.0f, lbox_top + 8.0f ), theme::text_accent(), "aim assist" );
            dl->ChannelsMerge();

            const float rbox_top = TITLE_H + 14.0f;
            float ry = rbox_top + 30.0f;
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            ImGui::SetCursorPos(ImVec2(R_X + 12.0f, ry));
            slider_float("Strength X", &m_aim.strength_x, 1.0f, 15.0f, "", "%.1f");
            ry = ImGui::GetCursorPos().y + 3.0f;
            ImGui::SetCursorPos(ImVec2(R_X + 12.0f, ry));
            slider_float("Strength Y", &m_aim.strength_y, 1.0f, 15.0f, "", "%.1f");
            ry = ImGui::GetCursorPos().y + 3.0f;
            ImGui::SetCursorPos(ImVec2(R_X + 12.0f, ry));
            slider_float("Aim Lerp", &m_aim.aim_lerp, 0.15f, 0.40f, "", "%.2f");
            ry = ImGui::GetCursorPos().y + 3.0f;
            ImGui::SetCursorPos(ImVec2(R_X + 12.0f, ry));
            slider_float("Aim Window (ms)", &m_aim.aim_window, 75.f, 110.f, "", "%.0f");
            ry = ImGui::GetCursorPos().y + 3.0f;
            ImGui::SetCursorPos(ImVec2(R_X + 12.0f, ry));
            slider_float("Decay Far", &m_aim.decay_far, 0.01f, 0.50f, "", "%.2f");
            ry = ImGui::GetCursorPos().y + 3.0f;
            ImGui::SetCursorPos(ImVec2(R_X + 12.0f, ry));
            slider_float("Freeze Lerp", &m_aim.freeze_lerp, 0.10f, 0.30f, "", "%.2f");
            ry = ImGui::GetCursorPos().y + 3.0f;
            ImGui::SetCursorPos(ImVec2(R_X + 12.0f, ry));

            ry = ImGui::GetCursorPos().y + 3.0f;
            ImGui::SetCursorPos(ImVec2(R_X + 12.0f, ry));
            if (m_aim.legit_mode) {
                ImGui::SetCursorPos(ImVec2(R_X + 12.0f, ry));
                slider_float("Legit Clamp", &m_aim.legit_clamp, 1.05f, 1.80f, "", "%.2f");
                ry = ImGui::GetCursorPos().y + 3.0f;
            }
            const float rbox_bottom = ry + 12.0f;
            dl->ChannelsSetCurrent(0);
            draw_glass_card( dl, S(R_X, rbox_top), S(R_X + R_W, rbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( R_X + 14.0f, rbox_top + 8.0f ), theme::text_accent(), "aim assist tune" );
            dl->ChannelsMerge();
        }
        else         if ( m_tab == 1 ) {
            const float lbox_top = TITLE_H + 14.0f + content_slide + replay_banner_h;
            float ly = lbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            checkbox( "Enable relax", &m_relax.enabled );
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            slider_int( "Manual offset", &m_relax.manual_offset_ms, -100, 100, " ms" );
            ly = ImGui::GetCursorPos( ).y + 8.0f;

            { bool is_singletap = ( m_relax.tap_style == 1 );
              ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
              checkbox( "Singletap mode", &is_singletap );
              m_relax.tap_style = is_singletap ? 1 : 0; }
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            slider_int( "Max ST BPM", &m_relax.singletap_bpm_cap, 100, 300, " bpm" );
            ly = ImGui::GetCursorPos( ).y;

            const float lbox_bottom = ly + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(L_X, lbox_top), S(L_X + L_W, lbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( L_X + 14.0f, lbox_top + 8.0f ), theme::text_accent(), "relax options" );
            dl->ChannelsMerge( );

            const float rbox_top = TITLE_H + 14.0f;
            float ry = rbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            dl->AddText( S( R_X + 14.0f, ry ), theme::text_bright(), "K1 Hold Shape" );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
            slider_float( "K1 Center", &m_relax.k1_hold_center, 30.f, 120.f, " ms" );
            ry = ImGui::GetCursorPos( ).y + 3.0f;

            ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
            slider_float( "K1 Spread", &m_relax.k1_hold_spread, 2.f, 30.f, " ms" );
            ry = ImGui::GetCursorPos( ).y + 8.0f;

            dl->AddText( S( R_X + 14.0f, ry ), theme::text_bright(), "K2 Hold Shape" );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
            slider_float( "K2 Center", &m_relax.k2_hold_center, 30.f, 120.f, " ms" );
            ry = ImGui::GetCursorPos( ).y + 3.0f;

            ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
            slider_float( "K2 Spread", &m_relax.k2_hold_spread, 2.f, 30.f, " ms" );
            ry = ImGui::GetCursorPos( ).y + 8.0f;

            ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
            slider_float( "Hold floor", &m_relax.hold_floor, 10.f, 60.f, " ms" );
            ry = ImGui::GetCursorPos( ).y + 3.0f;

            ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
            slider_float( "Hold ceiling", &m_relax.hold_ceiling, 60.f, 150.f, " ms" );
            ry = ImGui::GetCursorPos( ).y + 8.0f;

            if ( m_relax.is_active( ) )
                dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 100, 230, 160, 255 ), "Status: Running" );
            else if ( m_relax.is_synced( ) && m_relax.enabled )
                dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 230, 200, 100, 255 ), "Status: Synced" );
            else
                dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim(), "Status: Idle" );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            if ( m_relax.enabled && snap.beatmap.loaded ) {
                char buf[ 64 ];
                sprintf_s( buf, "Hit object: %d / %zu", m_relax.last_hit_obj_idx( ), snap.beatmap.objects.size( ) );
                dl->AddText( S( R_X + 14.0f, ry ), theme::text(), buf );
                ry += ImGui::GetTextLineHeight( );
            }

            const float rbox_bottom = ry + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(R_X, rbox_top), S(R_X + R_W, rbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( R_X + 14.0f, rbox_top + 8.0f ), theme::text_accent(), "hold times & status" );
            dl->ChannelsMerge( );
        }
        else if ( m_tab == 2 ) {
            const float lbox_top = TITLE_H + 14.0f + content_slide + replay_banner_h;
            float ly = lbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            checkbox( "Enable tap assist", &m_tap_assist.enabled );
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            checkbox( "Ignore sliders", &m_tap_assist.ignore_sliders );
            ly = ImGui::GetCursorPos( ).y;

            const float lbox_bottom = ly + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(L_X, lbox_top), S(L_X + L_W, lbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( L_X + 14.0f, lbox_top + 8.0f ), theme::text_accent(), "tap assist" );
            dl->ChannelsMerge( );

            const float rbox_top = TITLE_H + 14.0f;
            float ry = rbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
            slider_int( "Assist Window", &m_tap_assist.assist_window, 0, 250, " ms" );
            ry = ImGui::GetCursorPos( ).y + 3.0f;

            ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
            slider_int( "Randomization", &m_tap_assist.randomization, 0, 40, " ms" );
            ry = ImGui::GetCursorPos( ).y;

            const float rbox_bottom = ry + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(R_X, rbox_top), S(R_X + R_W, rbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( R_X + 14.0f, rbox_top + 8.0f ), theme::text_accent(), "tap assist tune" );
            dl->ChannelsMerge( );
        }
        else if ( m_tab == 3 ) {
            const float lbox_top = TITLE_H + 14.0f + content_slide + replay_banner_h;
            float ly = lbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            checkbox( "Enable replay bot", &m_replay.enabled );
            if ( ImGui::IsItemClicked( ) && m_replay.enabled ) m_replay.reset_sync( );
            ly = ImGui::GetCursorPos( ).y + 6.0f;

            dl->AddText( S( L_X + 14.0f, ly ), theme::text_dim(), "Replay path:" );
            ly += ImGui::GetTextLineHeight( ) + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            text_input( "##replay_path", m_replay_path_utf8, IM_ARRAYSIZE( m_replay_path_utf8 ), L_W - 24.0f );
            ly = ImGui::GetCursorPos( ).y + 6.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            if ( button( "Browse", L_W - 24.0f, 24.0f ) ) {
                OPENFILENAMEW ofn{};
                wchar_t file[ 512 ]{};
                ofn.lStructSize = sizeof( ofn );
                ofn.hwndOwner = m_hwnd;
                ofn.lpstrFilter = L"Replay Files\0*.osr\0All\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = 512;
                ofn.Flags = OFN_FILEMUSTEXIST;
                if ( GetOpenFileNameW( &ofn ) ) {
                    WideCharToMultiByte( CP_UTF8, 0, file, -1, m_replay_path_utf8, IM_ARRAYSIZE( m_replay_path_utf8 ), nullptr, nullptr );
                    m_replay.replay_path.assign( file );
                    m_replay.load_replay( );
                    m_replay.reset_sync( );
                }
            }
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            if ( button( "Load Replay", L_W - 24.0f, 24.0f ) ) {
                wchar_t wide[ 512 ]{};
                MultiByteToWideChar( CP_UTF8, 0, m_replay_path_utf8, -1, wide, 512 );
                m_replay.replay_path = wide;
                m_replay.load_replay( );
                m_replay.reset_sync( );
            }
            ly = ImGui::GetCursorPos( ).y;

            const float lbox_bottom = ly + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(L_X, lbox_top), S(L_X + L_W, lbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( L_X + 14.0f, lbox_top + 8.0f ), theme::text_accent(), "replay loading" );
            dl->ChannelsMerge( );

            const float rbox_top = TITLE_H + 14.0f;
            float ry = rbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
            checkbox( "Parse buttons", &m_replay.parse_buttons );
            ry = ImGui::GetCursorPos( ).y + 8.0f;

            char buf[ 64 ];
            sprintf_s( buf, "Frames: %zu", m_replay.frame_count( ) );
            dl->AddText( S( R_X + 14.0f, ry ), theme::text(), buf );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            sprintf_s( buf, "Valid: %s", m_replay.replay_valid( ) ? "yes" : "no" );
            dl->AddText( S( R_X + 14.0f, ry ), m_replay.replay_valid( ) ? IM_COL32( 100, 230, 160, 255 ) : IM_COL32( 255, 130, 130, 255 ), buf );
            ry += ImGui::GetTextLineHeight( );

            if ( !m_replay.last_load_error( ).empty( ) ) {
                ry += 4.0f;
                dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 255, 130, 130, 255 ), m_replay.last_load_error( ).c_str( ) );
                ry += ImGui::GetTextLineHeight( );
            }

            const float rbox_bottom = ry + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(R_X, rbox_top), S(R_X + R_W, rbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( R_X + 14.0f, rbox_top + 8.0f ), theme::text_accent(), "replay options" );
            dl->ChannelsMerge( );
        }
        else if ( m_tab == 4 ) {
            const float lbox_top = TITLE_H + 14.0f + content_slide + replay_banner_h;
            float ly = lbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            checkbox( "Enable autobot", &m_autobot.enabled );
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            slider_float( "Aim Spread", &m_autobot.aim_spread, 0.0f, 1.0f, "", "%.2f" );
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            slider_float( "Curve Strength", &m_autobot.curve_strength, 0.0f, 1.0f, "", "%.2f" );
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            slider_float( "Drift Amount", &m_autobot.drift_amount, 0.0f, 5.0f, "", "%.2f" );
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            slider_float( "Momentum", &m_autobot.momentum, 0.0f, 0.95f, "", "%.2f" );
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            slider_float( "Slider Laziness", &m_autobot.slider_laziness, 0.0f, 1.0f, "", "%.2f" );
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            slider_float( "Spinner RPM", &m_autobot.spinner_rpm, 200.0f, 477.0f, " rpm", "%.0f" );
            ly = ImGui::GetCursorPos( ).y;

            const float lbox_bottom = ly + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(L_X, lbox_top), S(L_X + L_W, lbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( L_X + 14.0f, lbox_top + 8.0f ), theme::text_accent(), "autobot options" );
            dl->ChannelsMerge( );

            const float rbox_top = TITLE_H + 14.0f;
            float ry = rbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            if ( m_autobot.enabled && snap.game.cur_state == osu::game_state_t::play )
                dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 100, 230, 160, 255 ), "Status: Running" );
            else
                dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim(), "Status: Idle" );
            ry += ImGui::GetTextLineHeight( ) + 8.0f;

            const float rbox_bottom = ry + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(R_X, rbox_top), S(R_X + R_W, rbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( R_X + 14.0f, rbox_top + 8.0f ), theme::text_accent(), "diagnostics" );
            dl->ChannelsMerge( );
        }
         else if ( m_tab == 5 ) {
             const float lbox_top = TITLE_H + 14.0f + content_slide + replay_banner_h;
            float ly = lbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            dl->AddText( S( L_X + 14.0f, ly ), theme::text_bright(), "Gameplay & Menu Keybinds:" );
            ly += ImGui::GetTextLineHeight( ) + 8.0f;

            char left_buf[ 16 ]{}, right_buf[ 16 ]{}, menu_buf[ 16 ]{};
            GetKeyNameTextA( MapVirtualKeyA( m_custom_left_key, MAPVK_VK_TO_VSC ) << 16, left_buf, sizeof( left_buf ) );
            GetKeyNameTextA( MapVirtualKeyA( m_custom_right_key, MAPVK_VK_TO_VSC ) << 16, right_buf, sizeof( right_buf ) );
            GetKeyNameTextA( MapVirtualKeyA( m_menu_keybind, MAPVK_VK_TO_VSC ) << 16, menu_buf, sizeof( menu_buf ) );
            if ( !left_buf[ 0 ] ) {
                if ( m_custom_left_key >= 32 && m_custom_left_key <= 126 ) sprintf_s( left_buf, "%c", m_custom_left_key );
                else sprintf_s( left_buf, "0x%02X", m_custom_left_key );
            }
            if ( !right_buf[ 0 ] ) {
                if ( m_custom_right_key >= 32 && m_custom_right_key <= 126 ) sprintf_s( right_buf, "%c", m_custom_right_key );
                else sprintf_s( right_buf, "0x%02X", m_custom_right_key );
            }
            if ( !menu_buf[ 0 ] ) {
                if ( m_menu_keybind >= 32 && m_menu_keybind <= 126 ) sprintf_s( menu_buf, "%c", m_menu_keybind );
                else sprintf_s( menu_buf, "0x%02X", m_menu_keybind );
            }

            if ( m_waiting_left ) {
                ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
                button( "Press key...##lbtn", L_W - 24.0f, 22.0f );
                for ( int k = 8; k < 256; ++k ) {
                    if ( k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON ) continue;
                    if ( GetAsyncKeyState( k ) & 0x8000 ) {
                        m_custom_left_key = k;
                        m_waiting_left = false;
                        if ( m_relax.is_active( ) ) {
                            osu::game_snapshot_t mod = snap.game;
                            apply_custom_keys( mod );
                            m_relax.on_leave_play( mod );
                        }
                        break;
                    }
                }
            }
            else {
                const std::string lbl = std::string( "Left Key: " ) + left_buf + "##lbtn";
                ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
                if ( button( lbl.c_str( ), L_W - 24.0f, 22.0f ) ) {
                    m_waiting_left = true; m_waiting_right = false; m_waiting_menu = false;
                }
            }
            ly += 26.0f;

            if ( m_waiting_right ) {
                ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
                button( "Press key...##rbtn", L_W - 24.0f, 22.0f );
                for ( int k = 8; k < 256; ++k ) {
                    if ( k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON ) continue;
                    if ( GetAsyncKeyState( k ) & 0x8000 ) {
                        m_custom_right_key = k;
                        m_waiting_right = false;
                        if ( m_relax.is_active( ) ) {
                            osu::game_snapshot_t mod = snap.game;
                            apply_custom_keys( mod );
                            m_relax.on_leave_play( mod );
                        }
                        break;
                    }
                }
            }
            else {
                const std::string lbl = std::string( "Right Key: " ) + right_buf + "##rbtn";
                ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
                if ( button( lbl.c_str( ), L_W - 24.0f, 22.0f ) ) {
                    m_waiting_right = true; m_waiting_left = false; m_waiting_menu = false;
                }
            }
            ly += 26.0f;

            if ( m_waiting_menu ) {
                ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
                button( "Press key...##menubtn", L_W - 24.0f, 22.0f );
                for ( int k = 8; k < 256; ++k ) {
                    if ( k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON ) continue;
                    if ( GetAsyncKeyState( k ) & 0x8000 ) {
                        m_menu_keybind = k;
                        m_waiting_menu = false;
                        break;
                    }
                }
            }
            else {
                const std::string lbl = std::string( "Menu Toggle Key: " ) + menu_buf + "##menubtn";
                ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
                if ( button( lbl.c_str( ), L_W - 24.0f, 22.0f ) ) {
                    m_waiting_menu = true; m_waiting_left = false; m_waiting_right = false;
                }
            }
            ly += 28.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            checkbox( "Stream proof", &stream_proof );
            ly = ImGui::GetCursorPos( ).y + 4.0f;

            const float lbox_bottom = ly + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(L_X, lbox_top), S(L_X + L_W, lbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( L_X + 14.0f, lbox_top + 8.0f ), theme::text_accent(), "gameplay bindings" );
            dl->ChannelsMerge( );

            const float rbox_top = TITLE_H + 14.0f;
            float ry = rbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            const char* client = "none";
            if ( snap.game.client == osu::client_kind_t::stable ) client = "osu!stable";
            else if ( snap.game.client == osu::client_kind_t::lazer ) client = "osu!lazer";

            const bool osu_wnd = input::target_window( ) && IsWindow( input::target_window( ) );

            char buf[ 128 ];
            sprintf_s( buf, "Osu window: %s", osu_wnd ? "found" : "not found" );
            dl->AddText( S( R_X + 14.0f, ry ), osu_wnd ? IM_COL32( 100, 230, 160, 255 ) : theme::text_dim(), buf );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            sprintf_s( buf, "Client: %s", client );
            dl->AddText( S( R_X + 14.0f, ry ), theme::text_bright(), buf );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            sprintf_s( buf, "Attached PID: %d", snap.game.pid );
            dl->AddText( S( R_X + 14.0f, ry ), theme::text(), buf );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            if ( snap.game.cur_state == osu::game_state_t::play )
                sprintf_s( buf, "Time: %d ms", snap.game.cur_time );
            else if ( snap.game.client == osu::client_kind_t::lazer && snap.game.diag_clock_ok )
                sprintf_s( buf, "Time: -- (raw %d ms)", snap.game.diag_raw_time );
            else sprintf_s( buf, "Time: --" );
            dl->AddText( S( R_X + 14.0f, ry ), theme::text(), buf );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            if ( snap.game.client == osu::client_kind_t::lazer ) {
                sprintf_s( buf, "Clock: %s | Beatmap: %s",
                    snap.game.diag_clock_ok ? "OK" : "FAIL",
                    snap.game.diag_beatmap_ok ? "OK" : "FAIL" );
                dl->AddText( S( R_X + 14.0f, ry ),
                    snap.game.diag_clock_ok ? theme::text() : IM_COL32( 255, 200, 100, 255 ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                sprintf_s( buf, "Stack: %s | count: %d",
                    snap.game.diag_stack_ok ? "OK" : "FAIL", snap.game.diag_stack_count );
                dl->AddText( S( R_X + 14.0f, ry ),
                    snap.game.diag_stack_ok ? theme::text() : IM_COL32( 255, 200, 100, 255 ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                sprintf_s( buf, "Screen: %s | Ruleset: %s",
                    snap.game.diag_current_screen_ok ? "OK" : "FAIL",
                    snap.game.diag_ruleset_ok ? "OK" : "FAIL" );
                dl->AddText( S( R_X + 14.0f, ry ),
                    ( snap.game.diag_current_screen_ok && snap.game.diag_ruleset_ok ) ? theme::text() : IM_COL32( 255, 200, 100, 255 ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                sprintf_s( buf, "API match: submit=%s player=%s",
                    snap.game.diag_submitting_api_match ? "YES" : "NO",
                    snap.game.diag_player_api_match ? "YES" : "NO" );
                dl->AddText( S( R_X + 14.0f, ry ),
                    ( snap.game.diag_submitting_api_match || snap.game.diag_player_api_match ) ? theme::text() : IM_COL32( 255, 200, 100, 255 ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                sprintf_s( buf, "Play gate: %s",
                    snap.game.cur_state == osu::game_state_t::play ? "OPEN" : "BLOCKED" );
                dl->AddText( S( R_X + 14.0f, ry ),
                    snap.game.cur_state == osu::game_state_t::play ? IM_COL32( 100, 230, 160, 255 ) : IM_COL32( 255, 200, 100, 255 ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                // Show the exact stage which failed. A non-null root pointer with
                // a null child means the child-field offset is stale; a null root
                // pointer points at the Game-object field offset instead.
                sprintf_s( buf, "Clock obj:%s final:%s",
                    snap.game.diag_beatmap_clock ? "YES" : "NO",
                    snap.game.diag_final_source ? "YES" : "NO" );
                dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim( ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                sprintf_s( buf, "Beatmap bind:%s value:%s",
                    snap.game.diag_beatmap_bindable ? "YES" : "NO",
                    snap.game.diag_working_beatmap ? "YES" : "NO" );
                dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim( ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                sprintf_s( buf, "ScreenStack obj:%s inner:%s",
                    snap.game.diag_screen_stack ? "YES" : "NO",
                    snap.game.diag_stack ? "YES" : "NO" );
                dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim( ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                sprintf_s( buf, "HELD clock: %s/%s time:%s",
                    snap.game.diag_hold_clock_obj ? "Y" : "N",
                    snap.game.diag_hold_clock_final ? "Y" : "N",
                    snap.game.diag_hold_clock_time ? "Y" : "N" );
                dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 150, 180, 255, 255 ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                sprintf_s( buf, "HELD beatmap: %s/%s | stack: %s/%s c=%d",
                    snap.game.diag_hold_beatmap_bind ? "Y" : "N",
                    snap.game.diag_hold_beatmap_value ? "Y" : "N",
                    snap.game.diag_hold_stack_obj ? "Y" : "N",
                    snap.game.diag_hold_stack_inner ? "Y" : "N",
                    snap.game.diag_hold_stack_count );
                dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 150, 180, 255, 255 ), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                if ( snap.game.diag_probe_stack_offset >= 0 ) {
                    sprintf_s( buf, "Stack probe: +0x%X count=%d api=%s ruleset=%s",
                        snap.game.diag_probe_stack_offset,
                        snap.game.diag_probe_stack_count,
                        snap.game.diag_probe_stack_api_match ? "YES" : "NO",
                        snap.game.diag_probe_stack_ruleset ? "YES" : "NO" );
                    dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 120, 210, 255, 255 ), buf );
                    ry += ImGui::GetTextLineHeight( ) + 4.0f;
                } else {
                    dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim( ), "Stack probe: NONE" );
                    ry += ImGui::GetTextLineHeight( ) + 4.0f;
                }

                if ( snap.game.diag_probe_bindable_offset >= 0 ) {
                    sprintf_s( buf, "Bindable probe: +0x%X score=%d map=%d",
                        snap.game.diag_probe_bindable_offset,
                        snap.game.diag_probe_bindable_score,
                        snap.game.diag_probe_map_id );
                    dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 120, 210, 255, 255 ), buf );
                    ry += ImGui::GetTextLineHeight( ) + 4.0f;

                    if ( !snap.game.diag_probe_difficulty.empty( ) ) {
                        sprintf_s( buf, "Beatmap probe name: %.40s", snap.game.diag_probe_difficulty.c_str( ) );
                        dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim( ), buf );
                        ry += ImGui::GetTextLineHeight( ) + 4.0f;
                    }
                } else {
                    dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim( ), "Bindable probe: NONE" );
                    ry += ImGui::GetTextLineHeight( ) + 4.0f;
                }

                if ( snap.game.diag_probe_clock_offset >= 0 ) {
                    sprintf_s( buf, "Clock probe: +0x%X value=%d hits=%d",
                        snap.game.diag_probe_clock_offset,
                        snap.game.diag_probe_clock_value,
                        snap.game.diag_probe_clock_hits );
                    dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 120, 210, 255, 255 ), buf );
                    ry += ImGui::GetTextLineHeight( ) + 4.0f;
                } else {
                    dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim( ), "Clock probe: NONE" );
                    ry += ImGui::GetTextLineHeight( ) + 4.0f;
                }

                if ( snap.game.diag_probe_clock_source_offset >= 0 ) {
                    sprintf_s( buf, "Clock chain: src+0x%X time+0x%X v=%d h=%d",
                        snap.game.diag_probe_clock_source_offset,
                        snap.game.diag_probe_clock_time_offset,
                        snap.game.diag_probe_clock_chain_value,
                        snap.game.diag_probe_clock_chain_hits );
                    dl->AddText( S( R_X + 14.0f, ry ), IM_COL32( 120, 210, 255, 255 ), buf );
                    ry += ImGui::GetTextLineHeight( ) + 4.0f;
                } else {
                    dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim( ), "Clock chain: NONE" );
                    ry += ImGui::GetTextLineHeight( ) + 4.0f;
                }

                if ( snap.game.diag_probe_capture_age_ms >= 0 ) {
                    sprintf_s( buf, "Probe hold: %d ms ago", snap.game.diag_probe_capture_age_ms );
                    dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim( ), buf );
                    ry += ImGui::GetTextLineHeight( ) + 4.0f;
                }
            }

            sprintf_s( buf, "Aim mouse hook: %s", m_mouse_hook.installed( ) ? "active" : "failed (poll fallback)" );
            dl->AddText( S( R_X + 14.0f, ry ),
                m_mouse_hook.installed( ) ? IM_COL32( 100, 230, 160, 255 ) : IM_COL32( 255, 200, 100, 255 ), buf );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            sprintf_s( buf, "Mouse input: %s", input::using_nt_input( ) ? "win32u" : "SendInput" );
            dl->AddText( S( R_X + 14.0f, ry ), theme::text(), buf );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            if ( snap.beatmap.loaded ) {
                sprintf_s( buf, "CS: %.1f | OD: %.1f | AR: %.1f", snap.beatmap.cs, snap.beatmap.od, snap.beatmap.ar );
                dl->AddText( S( R_X + 14.0f, ry ), theme::text_bright(), buf );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;
            }

            if ( snap.game.client == osu::client_kind_t::stable ) {
                dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim(), "Songs path override:" );
                ry += ImGui::GetTextLineHeight( ) + 4.0f;

                ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
                text_input( "##songs_override", m_songs_path_utf8, IM_ARRAYSIZE( m_songs_path_utf8 ), R_W - 24.0f );
                ry = ImGui::GetCursorPos( ).y + 6.0f;

                ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
                if ( button( "Browse##songs", R_W - 24.0f, 22.0f ) ) {
                    BROWSEINFOW bi{};
                    wchar_t buffer[ MAX_PATH ]{};
                    bi.lpszTitle = L"Select osu! Songs folder";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    if ( PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW( &bi ) ) {
                        if ( SHGetPathFromIDListW( pidl, buffer ) )
                            WideCharToMultiByte( CP_UTF8, 0, buffer, -1, m_songs_path_utf8, IM_ARRAYSIZE( m_songs_path_utf8 ), nullptr, nullptr );
                        CoTaskMemFree( pidl );
                    }
                }
                ry = ImGui::GetCursorPos( ).y + 4.0f;

                ImGui::SetCursorPos( ImVec2( R_X + 12.0f, ry ) );
                if ( button( "Apply Override", R_W - 24.0f, 22.0f ) && m_cache && m_songs_path_utf8[ 0 ] ) {
                    wchar_t wide[ 512 ]{};
                    MultiByteToWideChar( CP_UTF8, 0, m_songs_path_utf8, -1, wide, 512 );
                    m_cache->stable_parser( ).set_songs_path( wide );
                    m_cache->invalidate_beatmap_cache( );
                }
                ry = ImGui::GetCursorPos( ).y;
            }

            const float rbox_bottom = ry + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(R_X, rbox_top), S(R_X + R_W, rbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( R_X + 14.0f, rbox_top + 8.0f ), theme::text_accent(), "system diagnostic" );
            dl->ChannelsMerge( );
        }
        else if ( m_tab == 6 ) {
            if ( m_config_profiles.empty( ) )
                m_config_profiles = config::list_profiles( );

            const float lbox_top = TITLE_H + 14.0f + content_slide;
            float ly = lbox_top + 30.0f;

            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            dl->AddText( S( L_X + 14.0f, ly ), theme::text_bright(), "Config name:" );
            ly += ImGui::GetTextLineHeight( ) + 6.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            text_input( "##cfg_name", m_config_name_utf8, IM_ARRAYSIZE( m_config_name_utf8 ), L_W - 24.0f );
            ly = ImGui::GetCursorPos( ).y + 8.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            if ( button( "Save", ( L_W - 36.0f ) * 0.5f, 24.0f ) ) {
                const std::string name = config::sanitize_name( m_config_name_utf8 );
                if ( name.empty( ) ) m_config_status = "Enter a config name first.";
                else if ( config::save_profile( name, capture_settings( ) ) ) {
                    m_config_status = "Saved \"" + name + "\".";
                    m_config_profiles = config::list_profiles( );
                    strncpy_s( m_config_name_utf8, name.c_str( ), _TRUNCATE );
                }
                else m_config_status = "Failed to save config.";
            }

            ImGui::SetCursorPos( ImVec2( L_X + 18.0f + ( L_W - 36.0f ) * 0.5f, ly ) );
            if ( button( "Refresh list", ( L_W - 36.0f ) * 0.5f, 24.0f ) ) {
                m_config_profiles = config::list_profiles( );
                m_config_status = "Profile list refreshed.";
            }
            ly = ImGui::GetCursorPos( ).y + 10.0f;

            dl->AddText( S( L_X + 14.0f, ly ), theme::text_bright(), "Saved configs:" );
            ly += ImGui::GetTextLineHeight( ) + 6.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            const float list_h = MENU_H - ly - 80.0f;
            if ( ImGui::BeginListBox( "##cfg_list", ImVec2( L_W - 24.0f, list_h ) ) ) {
                for ( int i = 0; i < static_cast<int>( m_config_profiles.size( ) ); ++i ) {
                    const bool selected = ( m_config_selected == i );
                    if ( ImGui::Selectable( m_config_profiles[ static_cast<size_t>( i ) ].c_str( ), selected ) ) {
                        m_config_selected = i;
                        strncpy_s( m_config_name_utf8, m_config_profiles[ static_cast<size_t>( i ) ].c_str( ), _TRUNCATE );
                    }
                }
                ImGui::EndListBox( );
            }
            ly = ImGui::GetCursorPos( ).y + 8.0f;

            ImGui::SetCursorPos( ImVec2( L_X + 12.0f, ly ) );
            if ( button( "Load", L_W - 24.0f, 24.0f ) ) {
                std::string name = config::sanitize_name( m_config_name_utf8 );
                if ( name.empty( ) && m_config_selected >= 0 && m_config_selected < static_cast<int>( m_config_profiles.size( ) ) )
                    name = m_config_profiles[ static_cast<size_t>( m_config_selected ) ];
                config::settings_t loaded{};
                if ( name.empty( ) )
                    m_config_status = "Select or enter a config name.";
                else if ( config::load_profile( name, loaded ) ) {
                    apply_settings( loaded );
                    strncpy_s( m_config_name_utf8, name.c_str( ), _TRUNCATE );
                    m_config_status = "Loaded \"" + name + "\".";
                }
                else m_config_status = "Config not found.";
            }
            ly = ImGui::GetCursorPos( ).y + 6.0f;

            if ( !m_config_status.empty( ) ) {
                dl->AddText( S( L_X + 14.0f, ly ), theme::text_dim(), m_config_status.c_str( ) );
                ly += ImGui::GetTextLineHeight( ) + 4.0f;
            }

            const float lbox_bottom = ly + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(L_X, lbox_top), S(L_X + L_W, lbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( L_X + 14.0f, lbox_top + 8.0f ), theme::text_accent(), "profiles" );
            dl->ChannelsMerge( );

            const float rbox_top = TITLE_H + 14.0f;
            float ry = rbox_top + 30.0f;
            dl->ChannelsSplit( 2 );
            dl->ChannelsSetCurrent( 1 );

            dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim(), "Saves all module settings," );
            ry += ImGui::GetTextLineHeight( ) + 2.0f;
            dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim(), "keys, replay path, and system options." );
            ry += ImGui::GetTextLineHeight( ) + 8.0f;

            char path_buf[ 512 ]{};
            WideCharToMultiByte( CP_UTF8, 0, config::configs_dir( ).wstring( ).c_str( ), -1, path_buf, sizeof( path_buf ), nullptr, nullptr );
            dl->AddText( S( R_X + 14.0f, ry ), theme::text_dim(), "Folder:" );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;
            dl->AddText( S( R_X + 14.0f, ry ), theme::text(), path_buf );
            ry += ImGui::GetTextLineHeight( ) + 4.0f;

            const float rbox_bottom = ry + 12.0f;
            dl->ChannelsSetCurrent( 0 );
            draw_glass_card( dl, S(R_X, rbox_top), S(R_X + R_W, rbox_bottom), 8.0f, theme::accent() );
            dl->AddText( S( R_X + 14.0f, rbox_top + 8.0f ), theme::text_accent(), "config files" );
            dl->ChannelsMerge( );
        }

        render_open_dropdown( );
        render_open_color_picker( );

        ImGui::SetCursorPos( ImVec2( wsize.x, wsize.y ) );
        ImGui::Dummy( ImVec2( 1.0f, 1.0f ) );

        ImGui::End( );
    }
}