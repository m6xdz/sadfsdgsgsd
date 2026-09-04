#pragma once

#include <imgui.h>
#include <cmath>
#include <unordered_map>

namespace ui {

    inline float g_delta_time = 0.0f;
    inline float g_time = 0.0f;

    struct c_anim_value {
        float current = 0.f;
        float target = 0.f;
        float speed = 8.f;

        void update( float dt ) {
            current += ( target - current ) * std::min( dt * speed, 1.f );
            if ( std::abs( current - target ) < 0.001f )
                current = target;
        }

        void snap( float v ) { current = v; target = v; }
        void set( float v ) { target = v; }
        float get() const { return current; }
        bool done() const { return std::abs( current - target ) < 0.001f; }
        float mapped( float out_min, float out_max ) const {
            return out_min + ( out_max - out_min ) * current;
        }
    };

    struct c_anim_spring {
        float value = 0.f;
        float velocity = 0.f;
        float target = 0.f;
        float stiffness = 120.f;
        float damping = 12.f;

        void update( float dt ) {
            const float diff = target - value;
            const float force = diff * stiffness - velocity * damping;
            velocity += force * dt;
            value += velocity * dt;
            if ( std::abs( diff ) < 0.001f && std::abs( velocity ) < 0.001f ) {
                value = target;
                velocity = 0.f;
            }
        }

        void snap( float v ) { value = v; target = v; velocity = 0.f; }
        void set( float v ) { target = v; }
        float get() const { return value; }
    };

    struct c_anim_color {
        float r = 0.f, g = 0.f, b = 0.f, a = 0.f;
        float tr = 0.f, tg = 0.f, tb = 0.f, ta = 0.f;
        float speed = 8.f;

        void update( float dt ) {
            const float f = std::min( dt * speed, 1.f );
            r += ( tr - r ) * f; g += ( tg - g ) * f;
            b += ( tb - b ) * f; a += ( ta - a ) * f;
            if ( std::abs( r - tr ) < 0.001f ) r = tr;
            if ( std::abs( g - tg ) < 0.001f ) g = tg;
            if ( std::abs( b - tb ) < 0.001f ) b = tb;
            if ( std::abs( a - ta ) < 0.001f ) a = ta;
        }

        void set( float nr, float ng, float nb, float na ) {
            tr = nr; tg = ng; tb = nb; ta = na;
        }

        ImU32 get_u32() const {
            return IM_COL32(
                static_cast<int>( r * 255.f ),
                static_cast<int>( g * 255.f ),
                static_cast<int>( b * 255.f ),
                static_cast<int>( a * 255.f ) );
        }
    };

    inline void tick_animations( float dt ) {
        g_delta_time = dt;
        g_time += dt;
    }

    inline float& tab_hover_anim( int idx ) {
        static std::unordered_map<int, float> anims;
        return anims[ idx ];
    }

    inline float g_tab_indicator_y = 0.f;
    inline float g_card_entrance_time = 0.f;
    inline int   g_last_tab = -1;

    inline void update_tab_transition( int current_tab, float dt ) {
        if ( current_tab != g_last_tab ) {
            g_card_entrance_time = 0.f;
            g_last_tab = current_tab;
        }
        g_card_entrance_time += dt;
    }

    inline float card_entrance( int card_index, float stagger_delay = 0.06f ) {
        float t = g_card_entrance_time - card_index * stagger_delay;
        t = std::clamp( t, 0.f, 1.f );
        float ease = 1.f - std::pow( 1.f - t, 3.f );
        return ease;
    }

    inline float breathe( float speed = 1.5f, float min_v = 0.85f, float max_v = 1.0f ) {
        float t = std::sin( g_time * speed ) * 0.5f + 0.5f;
        return min_v + ( max_v - min_v ) * t;
    }

}