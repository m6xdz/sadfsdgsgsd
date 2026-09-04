#pragma once

#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>

namespace dbg {

    inline std::mutex g_log_mutex;
    inline FILE* g_log_file = nullptr;

    inline void open( ) {
        if ( g_log_file )
            return;

        wchar_t path[ MAX_PATH ];
        GetModuleFileNameW( nullptr, path, MAX_PATH );

        // put log next to the exe
        wchar_t* last = wcsrchr( path, L'\\' );
        if ( last ) *( last + 1 ) = L'\0';
        wcscat_s( path, L"debug.log" );

        _wfopen_s( &g_log_file, path, L"w" );
    }

    inline void log( const char* fmt, ... ) {
        std::lock_guard lock( g_log_mutex );
        if ( !g_log_file )
            open( );
        if ( !g_log_file )
            return;

        SYSTEMTIME st;
        GetLocalTime( &st );
        fprintf( g_log_file, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds );

        va_list args;
        va_start( args, fmt );
        vfprintf( g_log_file, fmt, args );
        va_end( args );

        fprintf( g_log_file, "\n" );
        fflush( g_log_file );
    }

}
