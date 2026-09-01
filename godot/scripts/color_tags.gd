extends RefCounted
## CDDA colour tags -> BBCode, and a plain-text strip.
##
## The game marks up strings as `<color_c_light_green>have it</color>`, and for
## some panes that markup carries the information rather than decorating it: in a
## crafting requirement list, green versus red *is* the answer to "do I have this?"
## Every panel until now stripped the tags on the C++ side, which is right when
## the colour is decoration and wrong here.
##
## The names come from `string_from_color()` in src/color.cpp. They are curses
## colour names, so they are mapped to the Nocturne palette's nearest reading
## rather than to literal terminal RGB -- the point is that "have" and "missing"
## stay apart and legible on the panel's ground, not that they match a terminal.
##
## An unknown tag resolves to nothing, which leaves the text in the label's own
## colour. That is the right failure: unstyled but readable, never invisible.

const _COLORS := {
	"black": "#3f424d",
	"red": "#e39b93",
	"green": "#8fd3ac",
	"brown": "#c9a227",
	"blue": "#7ea6e0",
	"magenta": "#c98ede",
	"cyan": "#7fd0d6",
	"gray": "#9397ab",
	"light_gray": "#b2b6ca",
	"dark_gray": "#75798c",
	"light_red": "#f0b3ac",
	"light_green": "#a9e0c2",
	"light_blue": "#9fc0ea",
	"light_magenta": "#dcaceb",
	"light_cyan": "#a3e0e5",
	"yellow": "#dcc57e",
	"white": "#e9e9ed",
	"pink": "#eaa8c8",
	"unset": "",
}

## Turn `<color_c_x>…</color>` into `[color=#rrggbb]…[/color]`, escaping the
## BBCode metacharacter first so item names containing "[" survive.
static func to_bbcode(text: String) -> String:
	if text.is_empty():
		return ""
	var out := text.replace("[", "[lb]")
	var result := ""
	var i := 0
	while true:
		var open := out.find("<color_", i)
		if open < 0:
			result += out.substr(i)
			break
		var close := out.find(">", open)
		if close < 0:
			result += out.substr(i)
			break
		result += out.substr(i, open - i)
		var hex := _hex_for(out.substr(open + 7, close - open - 7))
		result += "[color=%s]" % hex if hex != "" else ""
		i = close + 1
	return result.replace("</color>", "[/color]")

## Drop the markup entirely, for somewhere that cannot render it.
static func strip(text: String) -> String:
	var result := ""
	var i := 0
	while true:
		var open := text.find("<color_", i)
		if open < 0:
			result += text.substr(i)
			break
		var close := text.find(">", open)
		if close < 0:
			result += text.substr(i)
			break
		result += text.substr(i, open - i)
		i = close + 1
	return result.replace("</color>", "")

## Resolve a bare CDDA colour name -- "c_light_green", as it arrives in a
## snapshot field rather than inside markup -- to a hex string. Empty when it is
## not one we know, which callers should read as "use your own default".
static func hex_for_name(name: String) -> String:
	return _hex_for(name)

## Strip the `c_`/`h_`/`i_` prefix and any `_<background>` suffix; what is left is
## the foreground name. A highlighted or inverted variant reads as its base
## colour, which loses the emphasis but never loses the meaning.
static func _hex_for(name: String) -> String:
	var n := name
	for prefix in ["c_", "h_", "i_"]:
		if n.begins_with(prefix):
			n = n.substr(prefix.length())
			break
	if _COLORS.has(n):
		return str(_COLORS[n])
	# "light_green_red" is light_green on a red ground: try progressively shorter
	# prefixes so a background-qualified name still finds its foreground.
	var parts := n.split("_")
	while parts.size() > 1:
		parts.remove_at(parts.size() - 1)
		var candidate := "_".join(parts)
		if _COLORS.has(candidate):
			return str(_COLORS[candidate])
	return ""
