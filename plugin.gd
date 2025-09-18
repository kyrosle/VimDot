@tool
extends EditorPlugin

var _native_plugin

func _enter_tree() -> void:
	_ensure_settings()
	if !_native_plugin:
		_native_plugin = NvimEditorPlugin.new()
		add_child(_native_plugin)
		_native_plugin.set_panel_visible(false)

func _exit_tree() -> void:
	if _native_plugin:
		_native_plugin.queue_free()
		_native_plugin = null

func _has_main_screen() -> bool:
	return true

func _get_plugin_name() -> String:
	return "Neovim"

func _get_plugin_icon() -> Texture2D:
	return get_editor_interface().get_base_control().get_theme_icon("Script", "EditorIcons")

func _make_visible(visible: bool) -> void:
	if _native_plugin:
		_native_plugin.set_panel_visible(visible)

func _ensure_settings() -> void:
	var changed := false
	changed = _migrate_setting("neovim/embed/hide_script_editor", "neovim/embed/hide_script_editor_experimental") or changed
	changed = _ensure_setting("neovim/embed/autostart", true) or changed
	changed = _ensure_setting("neovim/embed/command", "nvim") or changed
	changed = _ensure_setting("neovim/embed/extra_args", PackedStringArray()) or changed
	changed = _ensure_setting("neovim/embed/font_path", "res://addons/nvim_embed/assets/fonts/jetbrains/JetBrainsMonoNerdFontMono-Regular.ttf") or changed
	changed = _ensure_setting("neovim/embed/font_size", 14) or changed
	changed = _ensure_setting("neovim/embed/hide_script_editor_experimental", false) or changed
	changed = _ensure_setting("neovim/embed/debug_logging", false) or changed
	changed = _ensure_setting("neovim/embed/theme", "default") or changed
	if changed:
		ProjectSettings.save()

func _ensure_setting(path: String, default_value) -> bool:
	if not ProjectSettings.has_setting(path):
		ProjectSettings.set_setting(path, default_value)
		return true
	return false

func _migrate_setting(old_path: String, new_path: String) -> bool:
	if ProjectSettings.has_setting(old_path) and not ProjectSettings.has_setting(new_path):
		ProjectSettings.set_setting(new_path, ProjectSettings.get_setting(old_path))
		ProjectSettings.clear(old_path)
		return true
	return false
