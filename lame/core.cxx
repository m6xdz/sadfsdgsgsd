#include <impl/includes.hxx>
#include <impl/ui/overlay.hxx>
#include <core/threads/cache.hxx>
#include <mmsystem.h>

#pragma comment( lib, "winmm.lib" )

static void enable_process_dpi_awareness( ) {
    typedef BOOL( WINAPI* PFN_SetProcessDpiAwarenessContext )( void* );
    if ( auto user32 = GetModuleHandleW( L"user32.dll" ) ) {
        if (auto SetProcessDpiAwarenessContextFn = ( PFN_SetProcessDpiAwarenessContext )GetProcAddress( user32, "SetProcessDpiAwarenessContext" ) ) {
            SetProcessDpiAwarenessContextFn( ( void* ) -4);
            return;
        }
    }
    typedef HRESULT( WINAPI* PFN_SetProcessDpiAwareness )( int );
    if ( auto shcore = LoadLibraryW( L"shcore.dll") ) {
        if (auto SetProcessDpiAwarenessFn = ( PFN_SetProcessDpiAwareness )GetProcAddress( shcore, "SetProcessDpiAwareness" ) ) {
            SetProcessDpiAwarenessFn( 2 );
            FreeLibrary( shcore );
            return;
        }
        FreeLibrary( shcore );
    }
    SetProcessDPIAware( );
}

int WINAPI wWinMain( HINSTANCE instance, HINSTANCE, PWSTR, int ) {
    enable_process_dpi_awareness( );
    ::timeBeginPeriod( 1 );

    threads::c_cache cache;
    if ( !cache.init( ) ) {
        MessageBoxW( nullptr, L"init failed", L"lame", MB_ICONERROR | MB_OK );
        return 1;
    }

    ui::c_overlay overlay;
    if ( !overlay.create( instance ) ) {
        MessageBoxW( nullptr, L"create failed", L"lame", MB_ICONERROR | MB_OK );
        return 1;
    }

    overlay.set_cache( &cache );
    overlay.set_snapshot_source( [ &cache ]() {
        return cache.get_snapshot( );
    } );
    cache.set_module_tick( [ &overlay ]( const osu::game_snapshot_t& game, const osu::beatmap_data_t& beatmap ) {
        overlay.tick_modules( game, beatmap );
    } );

    cache.start( );

    while ( overlay.pump( ) ) {
        Sleep( 1 );
    }

    cache.stop( );
    overlay.destroy( );

    ::timeEndPeriod( 1 );
    return 0;
}