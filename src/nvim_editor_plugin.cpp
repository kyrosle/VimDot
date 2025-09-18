#include "nvim_editor_plugin.h"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/tab_container.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

void NvimEditorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_panel_visible", "visible"), &NvimEditorPlugin::set_panel_visible);
}

void NvimEditorPlugin::_enter_tree() {
	if (!panel) {
		panel = memnew(NvimPanel);
		panel->set_name("Neovim");
	}

	if (panel && !panel->is_inside_tree()) {
		EditorInterface *editor = get_editor_interface();
		if (editor) {
			Control *main_screen = editor->get_editor_main_screen();
			if (main_screen) {
				panel->set_h_size_flags(Control::SIZE_FILL | Control::SIZE_EXPAND);
				panel->set_v_size_flags(Control::SIZE_FILL | Control::SIZE_EXPAND);
				main_screen->add_child(panel);
				panel->hide();
			}
		}
	}

	add_tool_menu_item("Start Neovim", callable_mp(this, &NvimEditorPlugin::_tool_start_neovim));
	add_tool_menu_item("Stop Neovim", callable_mp(this, &NvimEditorPlugin::_tool_stop_neovim));
	add_tool_menu_item("Open Current File in Neovim", callable_mp(this, &NvimEditorPlugin::_tool_open_current_file));
	add_tool_menu_item("Send :w", callable_mp(this, &NvimEditorPlugin::_tool_send_write));
	_refresh_toggle_autostart_menu(_is_autostart_enabled());
	_sync_script_editor_visibility(_is_script_hijack_enabled());
}

void NvimEditorPlugin::_exit_tree() {
	remove_tool_menu_item("Start Neovim");
	remove_tool_menu_item("Stop Neovim");
	remove_tool_menu_item("Open Current File in Neovim");
	remove_tool_menu_item("Send :w");
	if (!toggle_autostart_menu_label.is_empty()) {
		remove_tool_menu_item(toggle_autostart_menu_label);
		toggle_autostart_menu_label = String();
	}
	_apply_script_editor_visibility(false);
	script_editor_visibility_initialized = false;
	script_editor_hidden_cached = false;
	if (panel) {
		panel->queue_free();
		panel = nullptr;
	}
}

String NvimEditorPlugin::_get_plugin_name() const {
	return "Neovim";
}

Ref<Texture2D> NvimEditorPlugin::_get_plugin_icon() const {
	EditorInterface *editor = const_cast<NvimEditorPlugin *>(this)->get_editor_interface();
	if (!editor) {
		return Ref<Texture2D>();
	}
	Control *base = editor->get_base_control();
	if (!base) {
		return Ref<Texture2D>();
	}
	return base->get_theme_icon("Script", "EditorIcons");
}

void NvimEditorPlugin::_make_visible(bool p_visible) {
	if (!panel) {
		return;
	}
	panel->set_visible(p_visible);
	if (p_visible) {
		panel->grab_focus();
	}
}

void NvimEditorPlugin::set_panel_visible(bool p_visible) {
	if (panel) {
		panel->set_visible(p_visible);
		if (p_visible) {
			panel->grab_focus();
		}
	}
}

bool NvimEditorPlugin::_handles(Object *p_object) const {
	bool should_hijack = _is_script_hijack_enabled();
	_sync_script_editor_visibility(should_hijack);
	if (!should_hijack || !p_object) {
		return false;
	}

	return p_object->is_class("Script");
}

void NvimEditorPlugin::_edit(Object *p_object) {
	if (!_is_script_hijack_enabled()) {
		return;
	}

	Script *script = Object::cast_to<Script>(p_object);
	if (!script) {
		return;
	}

	String resource_path = script->get_path();
	if (resource_path.is_empty()) {
		UtilityFunctions::printerr("[nvim_embed] Cannot open script without a file path in Neovim.");
		return;
	}

	if (!panel) {
		return;
	}

	if (!panel->is_running()) {
		panel->start_nvim();
		if (!panel->is_running()) {
			UtilityFunctions::printerr("[nvim_embed] Neovim did not start; cannot open script.");
			return;
		}
	}

	String disk_path = _resolve_editor_disk_path(resource_path);
	if (disk_path.is_empty()) {
		disk_path = resource_path;
	}

	if (!panel->open_file_in_nvim(disk_path)) {
		UtilityFunctions::printerr(String("[nvim_embed] Failed to open ") + disk_path + " in Neovim.");
		return;
	}

	set_panel_visible(true);
	EditorInterface *editor = get_editor_interface();
	if (editor) {
		editor->set_main_screen_editor(_get_plugin_name());
	}
	_sync_script_editor_visibility(true);
	panel->grab_focus();
}

void NvimEditorPlugin::_tool_start_neovim() {
	if (panel) {
		panel->reload_settings();
		panel->start_nvim();
		set_panel_visible(true);
	}
}

void NvimEditorPlugin::_tool_stop_neovim() {
	if (panel) {
		panel->stop_nvim();
	}
}

void NvimEditorPlugin::_tool_open_current_file() {
	if (!panel) {
		return;
	}

	EditorInterface *editor = get_editor_interface();
	if (!editor) {
		return;
	}

	bool hijack_enabled = _is_script_hijack_enabled();
	_sync_script_editor_visibility(hijack_enabled);

	if (!panel->is_running()) {
		panel->start_nvim();
		if (!panel->is_running()) {
			UtilityFunctions::printerr("[nvim_embed] Neovim is not running; could not open the current file.");
			return;
		}
	}

	String resource_path = editor->get_current_path();
	if (resource_path.is_empty()) {
		UtilityFunctions::print("[nvim_embed] No active file to open in Neovim.");
		return;
	}

	String disk_path = _resolve_editor_disk_path(resource_path);
	if (disk_path.is_empty()) {
		UtilityFunctions::printerr(String("[nvim_embed] Unable to resolve disk path for ") + resource_path);
		return;
	}

	if (!panel->open_file_in_nvim(disk_path)) {
		UtilityFunctions::printerr(String("[nvim_embed] Failed to send :edit command for ") + disk_path);
		return;
	}

	set_panel_visible(true);
	if (hijack_enabled) {
		editor->set_main_screen_editor(_get_plugin_name());
		_sync_script_editor_visibility(true);
	}
	panel->grab_focus();
}

void NvimEditorPlugin::_tool_send_write() {
	if (!panel) {
		return;
	}

	if (!panel->is_running()) {
		UtilityFunctions::print("[nvim_embed] Neovim is not running; start it before sending :w.");
		return;
	}

	if (!panel->send_command("write")) {
		UtilityFunctions::printerr("[nvim_embed] Failed to send :w to Neovim.");
	}
}

void NvimEditorPlugin::_tool_toggle_autostart() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		UtilityFunctions::printerr("[nvim_embed] ProjectSettings unavailable; cannot toggle autostart.");
		return;
	}

	bool new_value = !_is_autostart_enabled();
	ps->set_setting("neovim/embed/autostart", new_value);
	Error save_result = ps->save();
	if (save_result != OK) {
		UtilityFunctions::printerr(String("[nvim_embed] Failed to save ProjectSettings (error ") + String::num_int64(save_result) + ")");
	}

	if (panel) {
		panel->reload_settings();
	}

	_refresh_toggle_autostart_menu(new_value);
	UtilityFunctions::print(String("[nvim_embed] Neovim autostart ") + (new_value ? String("enabled") : String("disabled")));
}

void NvimEditorPlugin::_refresh_toggle_autostart_menu(bool p_enabled) {
	if (!toggle_autostart_menu_label.is_empty()) {
		remove_tool_menu_item(toggle_autostart_menu_label);
	}

	toggle_autostart_menu_label = String("Toggle Neovim Autostart (") + (p_enabled ? String("On") : String("Off")) + ")";
	add_tool_menu_item(toggle_autostart_menu_label, callable_mp(this, &NvimEditorPlugin::_tool_toggle_autostart));
}

bool NvimEditorPlugin::_is_autostart_enabled() const {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return true;
	}

	if (!ps->has_setting("neovim/embed/autostart")) {
		return true;
	}

	Variant value = ps->get_setting("neovim/embed/autostart");
	if (value.get_type() == Variant::BOOL) {
		return static_cast<bool>(value);
	}

	return true;
}

bool NvimEditorPlugin::_is_script_hijack_enabled() const {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return false;
	}

	const String setting_path = "neovim/embed/hide_script_editor_experimental";
	if (!ps->has_setting(setting_path)) {
		if (ps->has_setting("neovim/embed/hide_script_editor")) {
			Variant legacy = ps->get_setting("neovim/embed/hide_script_editor");
			ps->set_setting(setting_path, legacy);
			ps->clear("neovim/embed/hide_script_editor");
			ps->save();
		}
	}

	Variant value = ps->get_setting(setting_path);
	if (value.get_type() == Variant::BOOL) {
		return static_cast<bool>(value);
	}

	return false;
}

void NvimEditorPlugin::_apply_script_editor_visibility(bool p_hide) const {
	EditorInterface *editor = const_cast<NvimEditorPlugin *>(this)->get_editor_interface();
	if (!editor) {
		return;
	}

	ScriptEditor *script_editor = editor->get_script_editor();
	if (script_editor) {
		script_editor->set_visible(!p_hide);
	}

	TabContainer *main_tabs = Object::cast_to<TabContainer>(editor->get_editor_main_screen());
	if (!main_tabs) {
		return;
	}

	Control *script_control = script_editor;
	Control *panel_control = panel;
	int32_t script_idx = script_control ? main_tabs->get_tab_idx_from_control(script_control) : -1;
	if (script_idx >= 0) {
		main_tabs->set_tab_hidden(script_idx, p_hide);
		if (p_hide && main_tabs->get_current_tab() == script_idx && panel_control) {
			int32_t panel_idx = main_tabs->get_tab_idx_from_control(panel_control);
			if (panel_idx >= 0) {
				main_tabs->set_current_tab(panel_idx);
			}
		}
	}
}

void NvimEditorPlugin::_sync_script_editor_visibility(bool p_hide) const {
	if (!script_editor_visibility_initialized || script_editor_hidden_cached != p_hide) {
		script_editor_visibility_initialized = true;
		script_editor_hidden_cached = p_hide;
		_apply_script_editor_visibility(p_hide);
	}
}

String NvimEditorPlugin::_resolve_editor_disk_path(const String &p_resource_path) const {
	if (p_resource_path.is_empty()) {
		return p_resource_path;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return p_resource_path;
	}

	if (p_resource_path.begins_with("res://") || p_resource_path.begins_with("user://")) {
		return ps->globalize_path(p_resource_path);
	}

	return p_resource_path;
}
