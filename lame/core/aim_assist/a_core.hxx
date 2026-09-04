#pragma once

#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <limits>
#include <emmintrin.h>

namespace assist {

    struct waypoint_t {
        float   x = 0.f;
        float   y = 0.f;
        float   hit_radius = 0.f;
        int32_t start_time = 0;
        int32_t end_time = 0;
        int32_t game_time = 0;
        bool    alive = false;
        bool    is_slider = false;
    };

    struct config {
        bool    enabled = false;
        bool    legit_mode = true;
        bool    ignore_sliders = false;
        float   strength_x = 5.0f;
        float   strength_y = 4.5f;
        float   decay_far = 0.02f;
        float   aim_lerp = 0.24f;
        float   freeze_lerp = 0.17f;
        float   aim_window = 100.f;
        float   legit_clamp = 1.3f;
        float   speed_mult = 1.f;
    };

    struct trace {
        float drift_x = 0.f;
        float drift_y = 0.f;
        float last_x = 0.f;
        float last_y = 0.f;
        float delta_x = 0.f;
        float delta_y = 0.f;
        float lock_weight = 0.f;
        float pull_x = 0.f;
        float pull_y = 0.f;
        int32_t bind_id = 0;
        float bind_x = 0.f;
        float bind_y = 0.f;
        float hook_sq = 0.f;
        int32_t last_clock = 0;
    };

    inline float strip( float v ) {
        uint32_t bits;
        std::memcpy( &bits, &v, sizeof( bits ) );
        bits &= 0x7FFFFFFF;
        float out;
        std::memcpy( &out, &bits, sizeof( out ) );
        return out;
    }

    inline float saturate( float v ) {
        if ( v < 0.f ) return 0.f;
        if ( v > 1.f ) return 1.f;
        return v;
    }

    inline bool adjust(
        float sx, float sy,
        const waypoint_t* markers, size_t n_markers,
        const RECT& frame,
        const config& params,
        trace& mem,
        float* ox, float* oy ) {

        if ( !ox || !oy ) return false;

        if ( !params.enabled ) {
            mem = {};
            *ox = sx;
            *oy = sy;
            return true;
        }

        int32_t tick = 0;
        if ( markers && n_markers > 0 )
            tick = markers[ 0 ].game_time;

        float raw_span = 1000.0f / 133.0f;
        if ( mem.last_clock > 0 && tick >= mem.last_clock )
            raw_span = static_cast<float>( tick - mem.last_clock );
        mem.last_clock = tick;

        const float speed = ( params.speed_mult > 0.01f ) ? params.speed_mult : 1.f;
        const float interval = ( raw_span / speed ) * ( 133.0f / 1000.0f );

        const float pw = static_cast<float>( frame.right - frame.left );
        const float ph = static_cast<float>( frame.bottom - frame.top );
        const float canvas_h = ph * 0.8f;
        const float coeff = ( canvas_h * ( 4.f / 3.f ) ) / 512.f;

        const float ori_x = static_cast<float>( frame.left ) + ( pw - ( 512.f * coeff ) ) * 0.5f;
        float ori_y = static_cast<float>( frame.top ) + ( ph - ( 384.f * coeff ) ) * 0.5f;
        ori_y -= ph * 0.012f;

        const float local_x = sx - ori_x;
        const float local_y = sy - ori_y;

        const waypoint_t* pick = nullptr;
        if ( markers && n_markers > 0 ) {
            for ( size_t i = 0; i < n_markers; ++i ) {
                const auto& m = markers[ i ];
                if ( m.start_time > tick && ( !params.ignore_sliders || !m.is_slider ) ) {
                    if ( !pick || m.start_time < pick->start_time )
                        pick = &m;
                }
            }
        }

        const float grip_x = saturate( params.strength_x / 15.f );
        const float grip_y = saturate( params.strength_y / 15.f );

        if ( pick ) {
            const float spot_x = pick->x * coeff;
            const float spot_y = pick->y * coeff;
            const int32_t fresh = pick->alive ? pick->start_time : 0;

            if ( pick->start_time == mem.bind_id ) {
                mem.bind_x = spot_x;
                mem.bind_y = spot_y;
                mem.hook_sq = pick->hit_radius * pick->hit_radius;
            } else {
                mem.drift_x = mem.delta_x;
                mem.drift_y = mem.delta_y;
                mem.pull_x = 0.f;
                mem.pull_y = 0.f;
                mem.bind_id = fresh;
                mem.bind_x = spot_x;
                mem.bind_y = spot_y;
                mem.hook_sq = pick->hit_radius * pick->hit_radius;
            }

            const int32_t eta = pick->start_time - tick;
            const float off_x = mem.bind_x - ( local_x + mem.delta_x );
            const float off_y = mem.bind_y - ( local_y + mem.delta_y );
            const float dist_sq = off_x * off_x + off_y * off_y;

            const float damp_step = params.freeze_lerp * interval;

            if ( dist_sq > mem.hook_sq ) {
                mem.lock_weight -= damp_step;
                if ( mem.lock_weight < 0.f ) mem.lock_weight = 0.f;
            } else {
                mem.lock_weight += damp_step;
                if ( mem.lock_weight > 1.f ) mem.lock_weight = 1.f;
            }

            float target_blend_x = 0.f;
            float target_blend_y = 0.f;

            if ( eta <= static_cast<int32_t>( params.aim_window ) ) {
                const float slack = 1.f - mem.lock_weight;
                const float erosion = std::min( params.decay_far * interval * slack, 1.f );
                const float preserve = 1.f - erosion;

                mem.drift_x *= preserve;
                mem.drift_y *= preserve;

                if ( strip( mem.drift_x ) < 0.05f ) mem.drift_x = 0.f;
                if ( strip( mem.drift_y ) < 0.05f ) mem.drift_y = 0.f;

                target_blend_x = grip_x;
                target_blend_y = grip_y;

                if ( eta >= 0 ) {
                    const float factor = 1.f - ( static_cast<float>( eta ) / params.aim_window );
                    target_blend_x *= factor;
                    target_blend_y *= factor;
                }
            }

            const float lerp = std::min( params.aim_lerp * interval * ( 1.f - mem.lock_weight ), 1.f );
            mem.pull_x += ( target_blend_x - mem.pull_x ) * lerp;
            mem.pull_y += ( target_blend_y - mem.pull_y ) * lerp;

        } else {
            mem.lock_weight -= params.freeze_lerp * interval;
            if ( mem.lock_weight < 0.f ) mem.lock_weight = 0.f;

            if ( mem.bind_id != 0 ) {
                mem.drift_x = mem.delta_x;
                mem.drift_y = mem.delta_y;
                mem.pull_x = 0.f;
                mem.pull_y = 0.f;
                mem.bind_id = 0;
            }
        }

        const bool has_bind = ( mem.bind_id != 0 );
        const float anchor_x = local_x + mem.drift_x;
        const float anchor_y = local_y + mem.drift_y;

        float aim_x, aim_y;
        if ( has_bind ) {
            aim_x = ( mem.bind_x - anchor_x ) * mem.pull_x + anchor_x;
            aim_y = ( mem.bind_y - anchor_y ) * mem.pull_y + anchor_y;
        } else {
            aim_x = anchor_x;
            aim_y = anchor_y;
        }

        float inj_x = aim_x - local_x;
        float inj_y = aim_y - local_y;

        if ( params.legit_mode ) {
            const float walk_x = local_x - mem.last_x;
            const float walk_y = local_y - mem.last_y;
            const float ceil_x = strip( walk_x ) * params.legit_clamp;
            const float ceil_y = strip( walk_y ) * params.legit_clamp;

            float fresh_x = inj_x - mem.delta_x;
            float fresh_y = inj_y - mem.delta_y;

            fresh_x = std::clamp( fresh_x, -ceil_x, ceil_x );
            fresh_y = std::clamp( fresh_y, -ceil_y, ceil_y );

            mem.delta_x += fresh_x;
            mem.delta_y += fresh_y;
        } else {
            mem.delta_x = inj_x;
            mem.delta_y = inj_y;
        }

        mem.last_x = local_x;
        mem.last_y = local_y;

        *ox = ori_x + local_x + mem.delta_x;
        *oy = ori_y + local_y + mem.delta_y;

        return true;
    }
}