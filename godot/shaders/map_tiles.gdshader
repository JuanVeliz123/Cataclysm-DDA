shader_type canvas_item;

// Batched tileset draw (ADR-003 B1).
//
// One MultiMeshInstance2D per (layer, atlas) draws every tile of that layer in a
// single call. The per-instance transform places the tile; the atlas sub-rect
// travels in INSTANCE_CUSTOM as normalized atlas coordinates, so a whole layer
// shares one material and one texture bind.

// xy = sub-rect origin, zw = sub-rect size, both normalized to the atlas.
varying vec4 atlas_rect;
// Per-instance modulation, from map_draw_cmd::tint.
//
// This began as the whole of the lighting model, replacing SDL's five
// pre-tinted atlas variants. With the light pass below running it carries hue
// only -- the green cast of night-vision goggles -- and brightness comes from
// the texture instead.
varying vec4 tint;
// Position in the light texture, 0..1 across the published map extent.
varying vec2 light_uv;

// 1.0 / atlas size in pixels. Used to inset sampling by half a texel so a tile
// never picks up its neighbour across an atlas region boundary.
uniform vec2 atlas_texel = vec2(0.0);

// --- Light pass (SP-3 visibility / memory, SP-4 per-pixel light) -------------
//
// One texel per map tile, from src/godot_light_snapshot.*, bound twice at two
// filters because the two channels want opposite things:
//
//   vis_tex   nearest. Visibility is a per-tile fact. Interpolating it would
//             smear a remembered tile into the seen one beside it.
//   light_tex linear. Interpolating IS the feature: it turns light that the
//             game computes per tile into a gradient across the room, for free,
//             in the sampler.
uniform sampler2D vis_tex : filter_nearest, repeat_disable, hint_default_black;
uniform sampler2D light_tex : filter_linear, repeat_disable, hint_default_black;
uniform bool light_enabled = false;
// False for the layer the avatar draws on. The player is the viewpoint; dimming
// it to match the floor makes it hard to find on a dark screen.
uniform bool receives_light = true;
// World-space rect the light texture covers: origin, and 1.0 / size.
uniform vec2 light_origin = vec2(0.0);
uniform vec2 light_inv_size = vec2(0.0);

// Colour an unlit but currently-seen tile falls to. Night in CDDA is blue-grey,
// not black: black would make the map unreadable rather than dark.
uniform vec3 dark_color : source_color = vec3(0.17, 0.18, 0.26);
// Remembered tiles: drained of colour and pushed cool, so memory reads as
// memory without needing a tint value reserved for it.
uniform vec3 memory_color : source_color = vec3(0.34, 0.36, 0.46);
uniform float memory_saturation = 0.18;

// Fire's contribution (SP-6), from the light texture's third channel.
//
// The particles draw the flames; this is the light they cast. It is separate
// from the G channel because it has to flicker, and the game's own lightmap is
// only rebuilt once a turn -- a fire that brightens and dims on the turn clock
// looks like a bug rather than like a fire.
uniform vec3 fire_color : source_color = vec3(1.0, 0.58, 0.24);
uniform float fire_strength = 0.9;

// --- Sway (SP-7) -------------------------------------------------------------
//
// Foliage moves; walls do not. Which is which is decided in C++ and arrives as
// a bit in map_draw_cmd::rot_flags, which MapView turns into a separate batch --
// so this is a uniform, not a per-tile test, and a screen of terrain still costs
// the same number of draw calls.
//
// Being a plant is not sufficient. The flag is only set on sprites that overhang
// their tile cell, because shearing one that exactly fills its cell pulls its
// top edge away from the neighbour it has to meet: a field of grass comes apart
// into disconnected squares. See sprite_overhangs_cell in
// src/godot_map_snapshot.cpp.
//
// The displacement is applied to the quad's top two vertices only. That is
// where "masked to the upper pixels" comes from for free: the mask is
// 1.0 - VERTEX.y over a unit quad, so the sprite shears about its own base
// instead of sliding, and the bottom stays planted where the tile is.
uniform bool sway_enabled = false;
// Unit direction times 0..1 strength, from the weather manager.
uniform vec2 wind = vec2(0.0);
// Peak displacement as a fraction of the sprite's width.
uniform float sway_amount = 0.09;
uniform float sway_speed = 1.6;

// --- Palette swap (SP-8) -----------------------------------------------------
//
// A sprite drawn through a palette is treated as a grayscale master: its
// luminance indexes a colour ramp, so one zombie sprite serves any number of
// variants that differ only in colour. Row 0 of the texture is the identity
// ramp, so palette_row 0 is a no-op even if it is somehow sampled.
uniform sampler2D palette_tex : filter_linear, repeat_disable, hint_default_white;
// Row in palette_tex, 1-based; 0 disables the lookup entirely.
uniform int palette_row = 0;
// Height of palette_tex in texels, which is one more than the palette count.
uniform float palette_rows = 1.0;

void vertex() {
    atlas_rect = INSTANCE_CUSTOM;
    // COLOR here carries the MultiMesh per-instance colour.
    tint = COLOR;
    // MODEL_MATRIX folds in the MultiMesh instance transform, so this is the
    // vertex's world position -- which is what the light uniforms are expressed
    // in, and is unaffected by MapView's zoom and camera offset.
    vec2 world = (MODEL_MATRIX * vec4(VERTEX, 0.0, 1.0)).xy;
    light_uv = (world - light_origin) * light_inv_size;

    if (sway_enabled) {
        // Two sines at incommensurate rates, phased by world position. The
        // phase is what matters: without it every plant on screen bends in
        // lockstep, which reads as the camera moving rather than the grass.
        float phase = dot(world, vec2(0.021, 0.017));
        float n = sin(TIME * sway_speed + phase * 6.283) * 0.62
                + sin(TIME * sway_speed * 1.73 + phase * 13.1) * 0.38;
        float mask = clamp(1.0 - VERTEX.y, 0.0, 1.0);
        // Into pixels and back. VERTEX is normalised per sprite, so adding the
        // same offset to both axes moves a 32x64 sprite twice as far vertically
        // as horizontally, and a diagonal wind bends the tip along the wrong
        // diagonal. Amplitude stays proportional to the sprite's height,
        // because a tree's crown should travel further than a grass tip.
        vec2 sprite_px = max(atlas_rect.zw / max(atlas_texel, vec2(1e-5)), vec2(1.0));
        VERTEX += n * mask * wind * (sway_amount * sprite_px.y) / sprite_px;
    }
}

void fragment() {
    vec2 uv = atlas_rect.xy + clamp(UV, vec2(0.0), vec2(1.0)) * atlas_rect.zw;
    vec2 lo = atlas_rect.xy + atlas_texel * 0.5;
    vec2 hi = atlas_rect.xy + atlas_rect.zw - atlas_texel * 0.5;
    vec4 tex = texture(TEXTURE, clamp(uv, lo, hi));

    if (palette_row > 0) {
        float lum = dot(tex.rgb, vec3(0.299, 0.587, 0.114));
        tex.rgb = texture(palette_tex,
            vec2(lum, (float(palette_row) + 0.5) / palette_rows)).rgb;
    }

    vec3 col = tex.rgb * tint.rgb;

    if (light_enabled && receives_light) {
        vec2 luv = clamp(light_uv, vec2(0.0), vec2(1.0));
        float vis = texture(vis_tex, luv).r;
        float lum = texture(light_tex, luv).g;

        // LightSnapshot writes 255 for seen and 64 for remembered, so the
        // midpoint of this ramp separates them cleanly and nothing else lands
        // inside it.
        float mem = 1.0 - smoothstep(0.3, 0.8, vis);

        vec3 lit = col * mix(dark_color, vec3(1.0), clamp(lum, 0.0, 1.0));

        // Fire, blurred across neighbouring tiles by the C++ side so this
        // sample is already a gradient rather than a square.
        float fire = texture(light_tex, luv).b;
        if (fire > 0.004) {
            float f = sin(TIME * 9.1 + luv.x * 47.0) * 0.5
                    + sin(TIME * 13.7 + luv.y * 31.0) * 0.5;
            lit += col * fire * fire_strength * (0.78 + 0.22 * f) * fire_color;
        }

        float grey = dot(col, vec3(0.299, 0.587, 0.114));
        vec3 remembered = mix(vec3(grey), col, memory_saturation) * memory_color;
        col = mix(lit, remembered, mem);
    }

    COLOR = vec4(col, tex.a * tint.a);
}
