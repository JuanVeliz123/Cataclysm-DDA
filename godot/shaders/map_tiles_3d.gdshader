shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, blend_mix, ambient_light_disabled;

// The 3D backend's tile shader (ADR-006 item 3D-1), and a deliberate line-by-line
// port of map_tiles.gdshader rather than a rewrite.
//
// KEEP THE TWO IN STEP. The claim this backend is judged on is that a screenshot
// is indistinguishable from the 2D one, so every uniform, constant and expression
// below has to mean what its canvas counterpart means. Diff the two files when
// changing either.
//
// What is *not* a port, and why:
//
//   - **Lit, but CDDA still decides.** The colour this shader computes -- sprite,
//     tint, CDDA's per-tile light, memory, the grade -- goes to EMISSION, so a
//     sprite with no light near it comes out exactly as it did unshaded. Engine
//     lights then *add* a directional term in `light()`, masked by that same
//     per-tile light. The game keeps saying which tiles are lit and how much; the
//     renderer only says what that looks like from a direction. ADR-003 refused to
//     let Godot re-derive field of view, and this is the same refusal about light.
//
//     The flat world loses nothing by being lit: every normal there points at the
//     camera, so the added term is uniform and nearly invisible -- which is the
//     right amount of difference for a world with no shape.
//   - `ALPHA_SCISSOR_THRESHOLD`, which puts the sprites in the opaque pass with
//     per-pixel depth (3D-1b). That is what lets two sprites from different atlases
//     interleave without being in the same MultiMesh, and it is why map_view_3d.gd
//     needs neither the row nor the layer in its batch key -- about a dozen batches
//     against the 2D backend's few hundred in a forest. Each sprite carries its own
//     z, in the order depth_rank computes. Blending was the state the first
//     baseline was taken in; see `alpha_scissor` for how to get back to it, because
//     it is not just a matter of the threshold.
//   - `cull_disabled`. Screen space has y down and 3D has y up, so placing a tile
//     mirrors it; the mirror flips triangle winding and would leave every sprite
//     back-facing.

// xy = sub-rect origin, zw = sub-rect size, both normalized to the atlas.
varying vec4 atlas_rect;
// Per-instance modulation, from map_draw_cmd::tint.
varying vec4 tint;
// Position in the light texture, 0..1 across the published map extent.
varying vec2 light_uv;

// The atlas. A spatial shader has no TEXTURE built-in and MultiMeshInstance3D has
// no texture slot, so the atlas that MultiMeshInstance2D carried as a property is
// a uniform here. Nearest, matching `textures/canvas_textures/default_texture_filter`.
uniform sampler2D atlas_tex : filter_nearest, repeat_disable, hint_default_black;
// 1.0 / atlas size in pixels, for the half-texel inset.
uniform vec2 atlas_texel = vec2(0.0);

// Whether the render pipeline will convert this shader's output from linear to
// sRGB, so that the output has to be handed over decoded.
//
// This is the one thing in this file that could not be settled without a GPU, so
// it is a switch rather than a guess. The canvas pipeline treats colours as sRGB
// and writes them out unchanged; the 3D pipeline treats them as linear and encodes
// on output. The grade below was authored against the canvas, so it is computed in
// sRGB here too and then decoded, and the pipeline's encode cancels the decode --
// giving the same pixels. If this target turns out not to be converted (an HDR one
// may not be), the map comes out too dark and this goes false. One flag, one
// screenshot, one answer.
uniform bool pipeline_encodes_srgb = true;

// Alpha below this is discarded instead of blended, which is what puts this
// material in the opaque pass.
//
// Note what this uniform cannot do: writing ALPHA_SCISSOR_THRESHOLD at all is what
// makes the material a cutout, so lowering this to zero does not restore blending
// -- it makes every half-transparent pixel opaque instead, which is worse. Going
// back to blending is a two-line edit here: `depth_draw_never` in the render mode
// and no write to ALPHA_SCISSOR_THRESHOLD below. If the hard edges are the problem
// rather than the sorting, `depth_prepass_alpha` is the third option: blended
// edges, with a depth pre-pass doing the interleaving.
uniform float alpha_scissor = 0.5;

// --- Light pass (SP-3 visibility / memory, SP-4 per-pixel light) -------------
uniform sampler2D vis_tex : filter_nearest, repeat_disable, hint_default_black;
uniform sampler2D light_tex : filter_linear, repeat_disable, hint_default_black;
uniform bool light_enabled = false;
uniform bool receives_light = true;
// The rect the light texture covers, in *world* units. The map sits at the world
// origin and the camera does the panning and zooming, so this is the map's own
// extent -- see map_view_3d.gd's _update_uniforms.
uniform vec2 light_origin = vec2(0.0);
uniform vec2 light_inv_size = vec2(0.0);

// Which world plane a tile's identity lives in (3D-3).
//
// Flat, the map is the XY plane and a vertex's own y says which row it is in -- so a
// tall sprite's top samples the light of the tile above it, exactly as the canvas
// backend does. Stood up, rows run along Z and height runs along Y, so the row comes
// from z and the whole of a standing sprite samples the tile it stands on. That is
// also what SDL does, and it is the more correct of the two: a tree is lit by the
// ground it grows out of, not by the tile its canopy overhangs.
uniform bool world_tilted = false;

uniform vec3 dark_color : source_color = vec3(0.17, 0.18, 0.26);
uniform vec3 memory_color : source_color = vec3(0.34, 0.36, 0.46);
uniform float memory_saturation = 0.18;

uniform vec3 fire_color : source_color = vec3(1.0, 0.58, 0.24);
uniform float fire_strength = 0.9;
uniform float fire_glow_gain = 2.4;

// --- Presentation grade -----------------------------------------------------
uniform float daylight = 1.0;
uniform float precipitation = 0.0;
uniform float pain = 0.0;
uniform vec3 night_tint : source_color = vec3(0.62, 0.72, 1.0);
uniform float night_desaturation = 0.45;
uniform vec3 wet_tint : source_color = vec3(0.82, 0.88, 1.0);
uniform float wet_desaturation = 0.30;
uniform vec3 pain_tint : source_color = vec3(1.0, 0.35, 0.30);
uniform float vignette_strength = 0.35;

// --- Sway (SP-7) -------------------------------------------------------------
uniform bool sway_enabled = false;
uniform vec2 wind = vec2(0.0);
uniform float sway_amount = 0.09;
uniform float sway_speed = 1.6;

// --- Palette swap (SP-8) -----------------------------------------------------
uniform sampler2D palette_tex : filter_linear, repeat_disable, hint_default_white;
uniform int palette_row = 0;
uniform float palette_rows = 1.0;

// How much a level below the avatar's is dimmed, per level (3D-4).
//
// C++ used to bake this into every tint (`fog_for_depth`), which meant tuning it needed a
// rebuild and meant a renderer that also faded lower levels would fade them twice. The
// handshake -- `set_depth_fog_enabled` -- moves the job here, and moves the numbers
// somewhere a person with a running game can turn them (VER-1). The blue is kept highest
// of the three for the same reason it was in C++: what reaches the bottom of a stairwell
// is bounced and cold.
uniform vec3 level_fade : source_color = vec3(0.55, 0.58, 0.66);
// How many levels below the avatar this batch draws, 0 for its own.
uniform int level_below = 0;
// Level blocks in the light texture; see LightSnapshot::max_levels. The image holds one
// block of the view's rows per published level, deepest last, so a tile's own light is
// found by sliding V into its block rather than by stretching one block over the whole
// texture. Before this, a level below sat out the light pass entirely -- the texel above
// it belonged to the tile the avatar could see.
uniform float light_texture_levels = 1.0;

// How much the engine's lights add on top of what CDDA already says (3D-2). A guess,
// and one of the first things to put in front of someone with a running game (VER-1);
// zero is this shader as it was before the lights existed.
uniform float engine_light_gain = 0.9;

// sRGB -> linear, applied to a colour that was computed in sRGB space so the
// pipeline's linear -> sRGB on the way out lands back where it started. Not
// clamped at 1.0: the fire term is deliberately above it, and clamping here would
// throw away the only thing the glow pass keys on.
// Slide a within-level UV into this batch's block of the light texture.
vec2 level_uv(vec2 uv) {
	float levels = max(light_texture_levels, 1.0);
	return vec2(clamp(uv.x, 0.0, 1.0),
		(clamp(uv.y, 0.0, 1.0) + float(level_below)) / levels);
}

vec3 srgb_to_linear(vec3 c) {
	vec3 safe = max(c, vec3(0.0));
	vec3 low = safe / 12.92;
	vec3 high = pow((safe + vec3(0.055)) / 1.055, vec3(2.4));
	return mix(low, high, step(vec3(0.04045), safe));
}

void vertex() {
	atlas_rect = INSTANCE_CUSTOM;
	// COLOR carries the MultiMesh per-instance colour, as in the canvas shader.
	tint = COLOR;
	// World position of the vertex. y is negative-down here (see cull_disabled
	// above), and light_inv_size carries the matching sign.
	vec3 placed = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	// The plane the map lives in; see world_tilted.
	vec2 world = world_tilted ? vec2(placed.x, placed.z) : placed.xy;
	light_uv = (world - light_origin) * light_inv_size;

	if (sway_enabled) {
		float phase = dot(world, vec2(0.021, 0.017));
		float n = sin(TIME * sway_speed + phase * 6.283) * 0.62
				+ sin(TIME * sway_speed * 1.73 + phase * 13.1) * 0.38;
		// VERTEX.y runs 0 at the sprite's top edge to 1 at its base, exactly as
		// on the canvas: the quad's local space is unchanged and the mirror lives
		// in the instance transform. So this still shears about the base.
		float mask = clamp(1.0 - VERTEX.y, 0.0, 1.0);
		vec2 sprite_px = max(atlas_rect.zw / max(atlas_texel, vec2(1e-5)), vec2(1.0));
		VERTEX.xy += n * mask * wind * (sway_amount * sprite_px.y) / sprite_px;
	}
}

void fragment() {
	vec2 uv = atlas_rect.xy + clamp(UV, vec2(0.0), vec2(1.0)) * atlas_rect.zw;
	vec2 lo = atlas_rect.xy + atlas_texel * 0.5;
	vec2 hi = atlas_rect.xy + atlas_rect.zw - atlas_texel * 0.5;
	vec4 tex = texture(atlas_tex, clamp(uv, lo, hi));

	if (palette_row > 0) {
		float lum = dot(tex.rgb, vec3(0.299, 0.587, 0.114));
		tex.rgb = texture(palette_tex,
			vec2(lum, (float(palette_row) + 0.5) / palette_rows)).rgb;
	}

	vec3 col = tex.rgb * tint.rgb;

	if (light_enabled && receives_light) {
		vec2 luv = level_uv(light_uv);
		float vis = texture(vis_tex, luv).r;
		float lum = texture(light_tex, luv).g;

		float mem = 1.0 - smoothstep(0.3, 0.8, vis);

		vec3 lit = col * mix(dark_color, vec3(1.0), clamp(lum, 0.0, 1.0));

		float fire = texture(light_tex, luv).b;
		if (fire > 0.004) {
			float f = sin(TIME * 9.1 + luv.x * 47.0) * 0.5
					+ sin(TIME * 13.7 + luv.y * 31.0) * 0.5;
			lit += col * fire * fire_strength * (0.78 + 0.22 * f)
				 * fire_color * fire_glow_gain;
		}

		float grey = dot(col, vec3(0.299, 0.587, 0.114));
		vec3 remembered = mix(vec3(grey), col, memory_saturation) * memory_color;
		col = mix(lit, remembered, mem);
	}

	// --- Grade ---------------------------------------------------------
	float luma = dot(col, vec3(0.299, 0.587, 0.114));

	float night = 1.0 - clamp(daylight, 0.0, 1.0);
	col = mix(col, vec3(luma), night * night_desaturation) * mix(vec3(1.0), night_tint, night);

	float wet = clamp(precipitation, 0.0, 1.0);
	col = mix(col, vec3(luma), wet * wet_desaturation) * mix(vec3(1.0), wet_tint, wet);

	if (pain > 0.001) {
		float edge = distance(SCREEN_UV, vec2(0.5));
		col = mix(col, col * pain_tint, clamp(pain, 0.0, 1.0) * smoothstep(0.15, 0.75, edge));
	}

	float vig = 1.0 - vignette_strength * smoothstep(0.35, 0.95, distance(SCREEN_UV, vec2(0.5)));
	col *= vig;

	// Depth fade, compounding per level as SDL's overlay does -- it draws once per level
	// crossed, so two down is faded twice.
	for (int i = 0; i < level_below; i++) {
		col *= level_fade;
	}

	vec3 out_col = pipeline_encodes_srgb ? srgb_to_linear(col) : col;
	// EMISSION carries the whole of what this shader used to output, so a sprite with
	// no light near it comes out exactly as it did unshaded. ALBEDO is what the
	// engine's lights are allowed to modulate.
	EMISSION = out_col;
	ALBEDO = out_col;
	ALPHA = tex.a * tint.a;
	// Two-sided, because half of these placements have a negative determinant -- the
	// flat world mirrors y, and every mirrored sprite mirrors x -- and a flipped
	// normal is a surface lit from behind. Turning it to face the viewer is what
	// `cull_disabled` already implies about a quad.
	if (dot(NORMAL, VIEW) < 0.0) {
		NORMAL = -NORMAL;
	}
	// Assigning this at all is what makes the material a cutout rather than a
	// transparent one, so it is written unconditionally and disabled by value.
	ALPHA_SCISSOR_THRESHOLD = alpha_scissor;
}

// The engine's lights, added to CDDA's rather than replacing them.
//
// Masked by the same per-tile light the fragment stage reads, so a light cannot
// brighten a tile the game says is dark -- which is what a lamp on the far side of a
// wall would otherwise do, since a shadow map knows about geometry and CDDA knows
// about walls. Sampled again here rather than handed along, because a varying can only
// be written in the vertex stage and this is one fetch.
// `return` is rejected outright in this processor function, so the exemptions are a
// factor rather than an early exit: the avatar's own layers and anything below its
// level pass `receives_light = false` and multiply out to nothing.
void light() {
	float mask = receives_light ? 1.0 : 0.0;
	if (light_enabled) {
		mask *= texture(light_tex, level_uv(light_uv)).g;
	}
	float ndl = max(dot(normalize(NORMAL), normalize(LIGHT)), 0.0);
	DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * ATTENUATION * ndl * engine_light_gain * mask;
}
