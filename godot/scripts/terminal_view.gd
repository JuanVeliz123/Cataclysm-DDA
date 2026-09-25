extends Control
## Overlay for leftover C++/curses menus (eat, craft, examine, wait, …).
## Empty cells stay transparent so MapView shows through.

var _host: Node
var _font: FontFile
var _font_size: int = 14
var _palette: PackedColorArray = PackedColorArray()
var _draw_glyphs: bool = true
var overlay_mode: bool = true

## Ints per cell: codepoint, fg, bg, occupied.
## Must match ViewSnapshot::cell_stride in src/godot_view_snapshot.h.
const CELL_STRIDE := 4

func setup(host: Node) -> void:
	_host = host
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	_ensure_font()
	_ensure_palette()
	queue_redraw()

func _ensure_font() -> void:
	if _font != null:
		return
	_font = FontFile.new()
	var root := ProjectSettings.globalize_path("res://").simplify_path()
	var candidates: Array[String] = [
		root.path_join("../data/font/Terminus.ttf").simplify_path(),
		root.path_join("../data/font/unifont.ttf").simplify_path(),
		root.path_join("../data/font/Roboto-Medium.ttf").simplify_path(),
	]
	for path in candidates:
		if FileAccess.file_exists(path) and _font.load_dynamic_font(path) == OK:
			print("TerminalView font: ", path)
			return
	push_warning("TerminalView: no TTF found; using ThemeDB fallback font")
	_font = null

func _ensure_palette() -> void:
	if _host == null:
		return
	var rgba: PackedByteArray = _host.get_view_palette_rgba()
	_palette.resize(16)
	var fallback: Array[Color] = [
		Color(0, 0, 0), Color(0.7, 0, 0), Color(0, 0.7, 0), Color(0.7, 0.7, 0),
		Color(0, 0, 0.7), Color(0.7, 0, 0.7), Color(0, 0.7, 0.7), Color(0.75, 0.75, 0.75),
		Color(0.4, 0.4, 0.4), Color(1, 0.2, 0.2), Color(0.2, 1, 0.2), Color(1, 1, 0.2),
		Color(0.3, 0.3, 1), Color(1, 0.3, 1), Color(0.3, 1, 1), Color(1, 1, 1),
	]
	for i in 16:
		_palette[i] = fallback[i]
	if rgba.size() >= 16 * 4:
		for i in 16:
			var o := i * 4
			var c := Color8(rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3])
			if i == 0 or c.r + c.g + c.b > 0.02:
				_palette[i] = c

func refresh() -> void:
	if _host == null:
		return
	_ensure_palette()
	queue_redraw()

## Publish where the cell grid actually landed, so the C++ input bridge can turn
## mouse pixel positions into cell coordinates. `input_context` reads
## `input_event::mouse_pos` as cells in the Godot build, so without this every
## click in a curses overlay resolves to the wrong cell.
var _last_geometry := Vector4i(-1, -1, -1, -1)

func _publish_cell_geometry(origin: Vector2, draw_w: float, draw_h: float) -> void:
	if _host == null:
		return
	# _draw() is in local space; mouse events arrive in viewport space.
	var world := global_position + origin
	var geometry := Vector4i(
		int(round(world.x)), int(round(world.y)),
		int(round(draw_w)), int(round(draw_h))
	)
	if geometry == _last_geometry:
		return
	_last_geometry = geometry
	_host.set_terminal_cell_geometry(geometry.x, geometry.y, geometry.z, geometry.w)

func _draw() -> void:
	var area := size
	if area.x < 2.0 or area.y < 2.0:
		area = get_viewport_rect().size
	if area.x < 2.0 or area.y < 2.0:
		return
	if _host == null:
		return

	var cols: int = _host.get_view_cols()
	var rows: int = _host.get_view_rows()
	if cols <= 0 or rows <= 0:
		return

	var cell_w: float = float(_host.get_view_cell_width())
	var cell_h: float = float(_host.get_view_cell_height())
	if cell_w < 1.0:
		cell_w = 8.0
	if cell_h < 1.0:
		cell_h = 16.0

	var scale: float = minf(area.x / (cols * cell_w), area.y / (rows * cell_h))
	if scale <= 0.0:
		return
	var draw_w: float = cell_w * scale
	var draw_h: float = cell_h * scale
	_font_size = maxi(8, int(draw_h * 0.85))

	var cells: PackedInt32Array = _host.get_view_cells()
	var expected: int = cols * rows * CELL_STRIDE
	if cells.size() < expected:
		return

	var font: Font = _font if _font != null else ThemeDB.fallback_font
	var origin := Vector2(
		(area.x - cols * draw_w) * 0.5,
		(area.y - rows * draw_h) * 0.5
	)
	_publish_cell_geometry(origin, draw_w, draw_h)

	for y in rows:
		for x in cols:
			var i: int = (y * cols + x) * CELL_STRIDE
			var cp: int = cells[i]
			var fg_i: int = clampi(cells[i + 1], 0, 15)
			var bg_i: int = clampi(cells[i + 2], 0, 15)
			var occupied: bool = cells[i + 3] != 0
			if cp < 0 or cp > 0x10FFFF or (cp >= 0xD800 and cp <= 0xDFFF):
				cp = 0
			# Unclaimed cells are the only transparent ones. Inferring this from the
			# background colour instead made every menu interior see-through, because
			# in curses black is a real colour rather than "nothing here".
			if overlay_mode and not occupied:
				continue
			var rect := Rect2(origin.x + x * draw_w, origin.y + y * draw_h, draw_w + 0.5, draw_h + 0.5)
			var bg: Color = _palette[bg_i]
			bg.a = 0.94
			draw_rect(rect, bg)
			if cp <= 32:
				continue
			var fg: Color = _palette[fg_i]
			if fg.r + fg.g + fg.b < 0.05:
				fg = Color(0.85, 0.85, 0.85)
			if absf(fg.r - bg.r) + absf(fg.g - bg.g) + absf(fg.b - bg.b) < 0.25:
				fg = Color(0.95, 0.95, 0.9) if bg.r + bg.g + bg.b < 1.4 else Color(0.08, 0.08, 0.1)
			var glyph := String.chr(cp)
			if glyph.is_empty():
				continue
			if _draw_glyphs:
				draw_string(font, Vector2(rect.position.x, rect.position.y + draw_h * 0.82),
					glyph, HORIZONTAL_ALIGNMENT_LEFT, -1, _font_size, fg)
			else:
				draw_rect(Rect2(rect.position + Vector2(1, 1), rect.size - Vector2(2, 2)), fg)
