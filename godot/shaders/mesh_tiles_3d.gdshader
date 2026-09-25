shader_type spatial;
render_mode depth_draw_opaque, ambient_light_disabled;

// The terrain mesh library's shader (3D-8d): map_tiles_3d.gdshader with the
// atlas removed and a mesh put in its place.
//
// KEEP THE LIGHT PASS AND THE GRADE IN STEP WITH map_tiles_3d.gdshader. The
// claim a furniture mesh is judged on is that it sits in the same world as the
// sprites around it -- same CDDA light, same memory grey, same night grade --
// and every uniform below means exactly what its tile counterpart means. What
// is deliberately absent: the atlas and its half-texel clamp (colour is baked
// into vertex COLOR at import), sway (furniture does not bend), the palette
// swap and the alpha scissor (these meshes are opaque), and cull_disabled
// (real geometry has consistent winding and real backfaces to cull).
//
// The per-instance CDDA tint arrives in INSTANCE_CUSTOM rather than the
// MultiMesh colour channel, because COLOR is already carrying the material:
// a spatial shader gets one COLOR built-in, and a MultiMesh with use_colors
// would overwrite the vertex paint with the instance value.

varying vec4 tint;
varying vec2 light_uv;

// --- Light pass (SP-3 / SP-4), uniform for uniform the tile shader's --------
uniform sampler2D vis_tex : filter_nearest, repeat_disable, hint_default_black;
uniform sampler2D light_tex : filter_linear, repeat_disable, hint_default_black;
uniform bool light_enabled = false;
uniform bool receives_light = true;
uniform vec2 light_origin = vec2(0.0);
uniform vec2 light_inv_size = vec2(0.0);
uniform bool world_tilted = false;
uniform bool pipeline_encodes_srgb = true;

uniform vec3 dark_color : source_color = vec3(0.17, 0.18, 0.26);
uniform vec3 memory_color : source_color = vec3(0.34, 0.36, 0.46);
uniform float memory_saturation = 0.18;

uniform vec3 fire_color : source_color = vec3(1.0, 0.58, 0.24);
uniform float fire_strength = 0.9;
uniform float fire_glow_gain = 2.4;

// --- Presentation grade ------------------------------------------------------
uniform float daylight = 1.0;
uniform float precipitation = 0.0;
uniform float pain = 0.0;
uniform vec3 night_tint : source_color = vec3(0.62, 0.72, 1.0);
uniform float night_desaturation = 0.45;
uniform vec3 wet_tint : source_color = vec3(0.82, 0.88, 1.0);
uniform float wet_desaturation = 0.30;
uniform vec3 pain_tint : source_color = vec3(1.0, 0.35, 0.30);
uniform float vignette_strength = 0.35;

uniform vec3 level_fade : source_color = vec3(0.55, 0.58, 0.66);
uniform int level_below = 0;
uniform float light_texture_levels = 1.0;
uniform float engine_light_gain = 0.9;
// Unused by this shader, accepted so the one uniform-update loop can feed the
// tile batches and the mesh batches without asking which is which.
uniform vec2 wind = vec2(0.0);
uniform float palette_rows = 1.0;
uniform int palette_row = 0;
uniform float alpha_scissor = 0.5;
uniform bool sway_enabled = false;
uniform sampler2D palette_tex : filter_linear, repeat_disable, hint_default_white;
uniform sampler2D atlas_tex : filter_nearest, repeat_disable, hint_default_black;
uniform vec2 atlas_texel = vec2(0.0);

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
	tint = INSTANCE_CUSTOM;
	vec3 placed = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec2 world = world_tilted ? vec2(placed.x, placed.z) : placed.xy;
	light_uv = (world - light_origin) * light_inv_size;
}

void fragment() {
	vec3 col = COLOR.rgb * tint.rgb;

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

	// --- Grade, line for line the tile shader's -------------------------
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

	for (int i = 0; i < level_below; i++) {
		col *= level_fade;
	}

	vec3 out_col = pipeline_encodes_srgb ? srgb_to_linear(col) : col;
	EMISSION = out_col;
	ALBEDO = out_col;
	// No ALPHA write, deliberately: writing it at all moves the material into
	// the transparent pass, and these meshes are opaque. The one thing lost is
	// the occlusion fade a sprite carries in its tint's alpha when it stands in
	// front of the avatar -- worth revisiting if a wardrobe ever hides someone.
}

// The engine's lights on real geometry, masked by CDDA's per-tile light exactly
// as on the sprites; see map_tiles_3d.gdshader's light() for the reasoning.
void light() {
	float mask = receives_light ? 1.0 : 0.0;
	if (light_enabled) {
		mask *= texture(light_tex, level_uv(light_uv)).g;
	}
	float ndl = max(dot(normalize(NORMAL), normalize(LIGHT)), 0.0);
	DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * ATTENUATION * ndl * engine_light_gain * mask;
}
