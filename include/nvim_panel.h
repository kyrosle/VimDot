#ifndef NVIM_PANEL_H
#define NVIM_PANEL_H

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/center_container.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/font_file.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "nvim_client.h"
#include "mpack.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace godot {

class NvimPanel;

class NvimGridCanvas : public Control {
	GDCLASS(NvimGridCanvas, Control);

private:
	NvimPanel *panel = nullptr;

protected:
	static void _bind_methods();
	void _notification(int32_t p_what);

public:
	void _gui_input(const Ref<InputEvent> &p_event) override;
	void set_panel(NvimPanel *p_panel) { panel = p_panel; }
};

class NvimPanel : public Control {
	GDCLASS(NvimPanel, Control);

private:
	friend class NvimGridCanvas;

	struct NvimCell {
		String text = " ";
		int64_t hl_id = 0;
	};

	struct NvimGrid {
		int32_t columns = 0;
		int32_t rows = 0;
		std::vector<std::vector<NvimCell>> cells;
	};

	struct Highlight {
		Color foreground = Color(1, 1, 1, 1);
		Color background = Color(0, 0, 0, 1);
		bool has_foreground = false;
		bool has_background = false;
	};

	int64_t nvim_pid = -1;
	VBoxContainer *root = nullptr;
	String nvim_command = "nvim";

	std::unique_ptr<NvimClient> nvim_client;
	std::vector<uint8_t> stdout_buffer;
	uint32_t next_request_id = 1;
	int32_t grid_columns = 80;
	int32_t grid_rows = 24;
	std::unordered_map<int64_t, NvimGrid> grids;
	int64_t current_grid_id = 0;
	int64_t cursor_row = 0;
	int64_t cursor_column = 0;
	std::unordered_map<int64_t, Highlight> highlight_definitions;
	Color default_foreground = Color(1, 1, 1, 1);
	Color default_background = Color(0, 0, 0, 1);
	NvimGridCanvas *grid_canvas = nullptr;
	int32_t font_size = 14;
	mutable Ref<Font> cached_font;
	CenterContainer *status_overlay = nullptr;
	Label *status_label = nullptr;
	Button *status_button = nullptr;
	bool autostart = true;
	bool nvim_crashed = false;
	String font_path_setting;
	PackedStringArray extra_args_setting;
	String theme_name_setting = "default";
	Color theme_default_foreground = Color(1, 1, 1, 1);
	Color theme_default_background = Color(0, 0, 0, 1);
	String theme_colorscheme_name;
	bool debug_logging_enabled = false;


	void _ensure_ui_created();
	void _update_ui_state();
	void _poll_nvim();
	bool _try_process_message();
	void _send_ui_attach();
	void _handle_redraw(const mpack_node_t &p_batches_node);
	void _handle_redraw_event(const String &p_event_name, const mpack_node_t &p_args_node);
	void _handle_grid_resize(const mpack_node_t &p_args_node);
	void _handle_grid_clear(const mpack_node_t &p_args_node);
	void _handle_grid_destroy(const mpack_node_t &p_args_node);
	void _handle_grid_line(const mpack_node_t &p_args_node);
	void _handle_grid_cursor_goto(const mpack_node_t &p_args_node);
	void _handle_grid_scroll(const mpack_node_t &p_args_node);
	NvimGrid &_ensure_grid(int64_t p_grid_id, int32_t p_columns, int32_t p_rows);
	void _fill_row(std::vector<NvimCell> &p_row);
	void _handle_hl_attr_define(const mpack_node_t &p_args_node);
	void _handle_default_colors_set(const mpack_node_t &p_args_node);
	Color _color_from_rgb_value(int64_t p_value) const;
	Color _resolve_foreground(int64_t p_hl_id) const;
	Color _resolve_background(int64_t p_hl_id) const;
	Ref<Font> _obtain_font() const;
	int32_t _obtain_font_size() const;
	void _draw_grid(NvimGridCanvas *p_canvas);
	void _update_canvas_size();
	void _request_grid_redraw();
	bool _send_nvim_input(const String &p_keys);
	bool _send_nvim_command(const String &p_command);
	bool _send_nvim_input_mouse(const String &p_button, const String &p_action, const String &p_modifiers, int64_t p_grid, int64_t p_row, int64_t p_column);
	String _build_modifier_string(bool p_shift, bool p_ctrl, bool p_alt) const;
	bool _handle_gui_input(const Ref<InputEvent> &p_event);
	bool _handle_key_event(const Ref<InputEventKey> &p_key_event);
	bool _handle_mouse_button_event(const Ref<InputEventMouseButton> &p_mouse_event);
	bool _handle_mouse_motion_event(const Ref<InputEventMouseMotion> &p_motion_event);
	String _translate_key_event(const Ref<InputEventKey> &p_key_event) const;
	String _format_special_key(const String &p_key_name, bool p_shift, bool p_ctrl, bool p_alt) const;
	void _convert_position_to_cell(const Vector2 &p_position, int64_t &r_row, int64_t &r_column) const;
	void _sync_neovim_size_to_canvas();
	bool _send_ui_try_resize(int32_t p_columns, int32_t p_rows);
	void _on_canvas_resized();
	void _ensure_cell_metrics();
	String _get_default_font_path() const;
	void _on_status_button_pressed();
	String _quote_path_for_command(const String &p_path) const;
	void _reset_highlight_defaults();
	void _apply_theme_defaults(bool p_force_redraw);
	bool _load_theme_definition(const String &p_theme_name);
	Color _parse_theme_color(const String &p_value, bool &r_valid) const;
	void _apply_theme_to_running_instance();

	float cell_width = 8.0f;
	float cell_height = 16.0f;
	float cell_ascent = 12.0f;

protected:
	static void _bind_methods();

public:
	NvimPanel();
	~NvimPanel() override;

	void _ready() override;
	void _exit_tree() override;
	void _process(double p_delta) override;

	void set_nvim_command(const String &p_command);
	String get_nvim_command() const;

	bool is_running();
	void start_nvim();
	void stop_nvim();
	bool send_input(const String &p_keys);
	bool send_command(const String &p_command);
	bool open_file_in_nvim(const String &p_path);
	void reload_settings();
};

} // namespace godot

#endif // NVIM_PANEL_H
