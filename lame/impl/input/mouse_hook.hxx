#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <atomic>
#include <thread>

namespace input {

    class c_mouse_hook {
    public:
        using transform_fn = bool ( * )( void* ctx, POINT pen, const MSLLHOOKSTRUCT& raw, POINT* out );

        ~c_mouse_hook( ) {
            uninstall( );
        }

        void set_filter_injected_only( bool v ) {
            m_filter_injected = v;
        }

        void set_transform( transform_fn fn, void* ctx ) {
            m_transform_fn = fn;
            m_transform_ctx = ctx;
        }

        bool install( ) {
            if ( m_installed.load( ) )
                return true;

            m_stop.store( false );
            m_ready.store( false );
            m_thread = std::thread( [ this ] { hook_thread_main( ); } );

            for ( int i = 0; i < 200 && !m_ready.load( ); ++i )
                Sleep( 5 );

            return m_installed.load( );
        }

        void uninstall( ) {
            m_stop.store( true );
            if ( m_thread_id != 0 )
                PostThreadMessageW( m_thread_id, WM_QUIT, 0, 0 );

            if ( m_thread.joinable( ) )
                m_thread.join( );

            m_installed.store( false );
            m_ready.store( false );
            m_thread_id = 0;
            m_hook = nullptr;
            if ( s_active == this )
                s_active = nullptr;
        }

        [[nodiscard]] bool installed( ) const {
            return m_installed.load( );
        }

    private:
        inline static c_mouse_hook* s_active = nullptr;

        transform_fn m_transform_fn = nullptr;
        void*        m_transform_ctx = nullptr;
        bool         m_filter_injected = true;

        std::thread       m_thread;
        std::atomic<bool> m_stop{ false };
        std::atomic<bool> m_ready{ false };
        std::atomic<bool> m_installed{ false };
        DWORD             m_thread_id = 0;
        HHOOK             m_hook = nullptr;

        static bool is_move_message( DWORD msg ) {
            return msg == WM_MOUSEMOVE;
        }

        static LRESULT CALLBACK low_level_proc( int code, WPARAM wparam, LPARAM lparam ) {
            if ( code != HC_ACTION || !lparam )
                return CallNextHookEx( nullptr, code, wparam, lparam );

            auto* self = s_active;
            if ( !self || !self->m_hook )
                return CallNextHookEx( nullptr, code, wparam, lparam );

            auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>( lparam );
            
            if ( ms->dwExtraInfo == 0xDEADC0DE )
                return CallNextHookEx( self->m_hook, code, wparam, lparam );

            if ( self->m_transform_fn && is_move_message( static_cast<DWORD>( wparam ) ) ) {
                POINT out{};
                const POINT pen = ms->pt;
                if ( self->m_transform_fn( self->m_transform_ctx, pen, *ms, &out ) ) {
                    return 1;
                }
            }

            return CallNextHookEx( self->m_hook, code, wparam, lparam );
        }

        void hook_thread_main( ) {
            ::SetThreadPriority( ::GetCurrentThread( ), THREAD_PRIORITY_HIGHEST );
            m_thread_id = GetCurrentThreadId( );
            s_active = this;

            m_hook = SetWindowsHookExW(
                WH_MOUSE_LL,
                low_level_proc,
                GetModuleHandleW( nullptr ),
                0 );

            if ( !m_hook ) {
                if ( s_active == this )
                    s_active = nullptr;
                m_installed.store( false );
                m_ready.store( true );
                return;
            }

            m_installed.store( true );
            m_ready.store( true );

            MSG msg{};
            while ( !m_stop.load( ) && GetMessageW( &msg, nullptr, 0, 0 ) > 0 ) {
                TranslateMessage( &msg );
                DispatchMessageW( &msg );
            }

            if ( m_hook ) {
                UnhookWindowsHookEx( m_hook );
                m_hook = nullptr;
            }

            if ( s_active == this )
                s_active = nullptr;

            m_installed.store( false );
        }
    };

}