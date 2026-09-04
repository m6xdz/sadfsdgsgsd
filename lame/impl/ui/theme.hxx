#pragma once

#include <imgui.h>
#include <cmath>

namespace ui::theme {

    inline ImVec4 accent_v4() { return ImVec4( 0.42f, 0.77f, 1.0f, 1.0f ); }   
    inline ImU32 accent() { return IM_COL32( 107, 197, 255, 255 ); }
    inline ImU32 accent_alpha( float a ) { return IM_COL32( 107, 197, 255, static_cast<int>( a * 255 ) ); }
    inline ImU32 accent_dim() { return IM_COL32( 60, 130, 200, 255 ); }

    inline ImU32 text() { return IM_COL32( 200, 210, 230, 255 ); }
    inline ImU32 text_bright() { return IM_COL32( 235, 240, 250, 255 ); }
    inline ImU32 text_dim() { return IM_COL32( 110, 120, 140, 255 ); }
    inline ImU32 text_accent() { return IM_COL32( 107, 197, 255, 255 ); }

    inline ImU32 track_bg() { return IM_COL32( 20, 20, 40, 200 ); }
    inline ImU32 track_fill() { return IM_COL32( 107, 197, 255, 200 ); }
    inline ImU32 handle() { return IM_COL32( 235, 240, 250, 255 ); }

    inline ImU32 tab_active_bg() { return IM_COL32( 107, 197, 255, 25 ); }
    inline ImU32 tab_hover_bg() { return IM_COL32( 107, 197, 255, 15 ); }

    inline float ease_out_cubic( float t ) {
        t = std::clamp( t, 0.f, 1.f );
        const float f = t - 1.f;
        return f * f * f + 1.f;
    }

    inline float ease_in_out( float t ) {
        t = std::clamp( t, 0.f, 1.f );
        return t < 0.5f ? 2.f * t * t : 1.f - std::pow( -2.f * t + 2.f, 2.f ) * 0.5f;
    }

    inline float ease_out_back( float t ) {
        const float c1 = 1.70158f;
        const float c3 = c1 + 1.f;
        t = std::clamp( t, 0.f, 1.f );
        return 1.f + c3 * std::pow( t - 1.f, 3.f ) + c1 * std::pow( t - 1.f, 2.f );
    }

    inline ImU32 lerp_color( ImU32 a, ImU32 b, float t ) {
        const float ta = static_cast<float>( ( a >> 24 ) & 0xFF );
        const float tr = static_cast<float>( ( a >> 16 ) & 0xFF );
        const float tg = static_cast<float>( ( a >> 8  ) & 0xFF );
        const float tb = static_cast<float>( ( a       ) & 0xFF );
        const float ba = static_cast<float>( ( b >> 24 ) & 0xFF );
        const float br = static_cast<float>( ( b >> 16 ) & 0xFF );
        const float bg = static_cast<float>( ( b >> 8  ) & 0xFF );
        const float bb = static_cast<float>( ( b       ) & 0xFF );
        return IM_COL32(
            static_cast<int>( tr + ( br - tr ) * t ),
            static_cast<int>( tg + ( bg - tg ) * t ),
            static_cast<int>( tb + ( bb - tb ) * t ),
            static_cast<int>( ta + ( ba - ta ) * t ) );
    }

}