#ifndef NVIM_EDITOR_PLUGIN_H
#define NVIM_EDITOR_PLUGIN_H

#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "nvim_panel.h"

namespace godot {

class NvimEditorPlugin : public EditorPlugin {
	GDCLASS(NvimEditorPlugin, EditorPlugin);

private:
	NvimPanel *panel = nullptr;
	String toggle_autostart_menu_label;
	mutable bool script_editor_visibility_initialized = false;
	mutable bool script_editor_hidden_cached = false;

	void _refresh_toggle_autostart_menu(bool p_enabled);
	bool _is_autostart_enabled() const;
	bool _is_script_hijack_enabled() const;
	void _apply_script_editor_visibility(bool p_hide) const;
	void _sync_script_editor_visibility(bool p_hide) const;
	String _resolve_editor_disk_path(const String &p_resource_path) const;

protected:
	static void _bind_methods();

public:
	NvimEditorPlugin() = default;
	~NvimEditorPlugin() override = default;

	void _enter_tree() override;
	void _exit_tree() override;
	bool _handles(Object *p_object) const override;
	void _edit(Object *p_object) override;
	bool _has_main_screen() const override { return true; }
	String _get_plugin_name() const override;
	Ref<Texture2D> _get_plugin_icon() const override;
	void _make_visible(bool p_visible) override;
	void set_panel_visible(bool p_visible);
	void _tool_start_neovim();
	void _tool_stop_neovim();
	void _tool_open_current_file();
	void _tool_send_write();
	void _tool_toggle_autostart();
};

} // namespace godot

#endif // NVIM_EDITOR_PLUGIN_H
