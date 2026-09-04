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

    class c_keyboard_hook {
    public:
        using key_event_fn = bool ( * )( void* ctx, int vk, bool is_down, int64_t press_qpc );

        ~c_keyboard_hook( ) {
            uninstall( );
        }

        void set_callback( key_event_fn fn, void* ctx ) {
            m_callback_fn = fn;
            m_callback_ctx = ctx;
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
        inline static c_keyboard_hook* s_active = nullptr;

        key_event_fn m_callback_fn = nullptr;
        void*        m_callback_ctx = nullptr;

        std::thread       m_thread;
        std::atomic<bool> m_stop{ false };
        std::atomic<bool> m_ready{ false };
        std::atomic<bool> m_installed{ false };
        DWORD             m_thread_id = 0;
        HHOOK             m_hook = nullptr;

        static LRESULT CALLBACK low_level_proc( int code, WPARAM wparam, LPARAM lparam ) {
            if ( code != HC_ACTION || !lparam )
                return CallNextHookEx( nullptr, code, wparam, lparam );

            auto* self = s_active;
            if ( !self || !self->m_hook )
                return CallNextHookEx( nullptr, code, wparam, lparam );

            auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>( lparam );

            const bool is_down = ( wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN );
            const bool is_up = ( wparam == WM_KEYUP || wparam == WM_SYSKEYUP );

            if ( kb->flags & LLKHF_INJECTED ) {
                return CallNextHookEx( self->m_hook, code, wparam, lparam );
            }

            if ( self->m_callback_fn && ( is_down || is_up ) ) {
                LARGE_INTEGER press_time;
                QueryPerformanceCounter( &press_time );
                if ( self->m_callback_fn( self->m_callback_ctx, static_cast<int>( kb->vkCode ), is_down, press_time.QuadPart ) ) {
                    return 1;
                }
            }

            return CallNextHookEx( self->m_hook, code, wparam, lparam );
        }

        void hook_thread_main( ) {
            m_thread_id = GetCurrentThreadId( );
            s_active = this;

            m_hook = SetWindowsHookExW(
                WH_KEYBOARD_LL,
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