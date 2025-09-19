#include "nvim_panel.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>

#include <godot_cpp/classes/box_container.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/theme_db.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "mpack.h"

using namespace godot;

namespace {
constexpr int64_t INVALID_PID = -1;
}

void NvimGridCanvas::_bind_methods() {}

void NvimGridCanvas::_notification(int32_t p_what) {
	if (!panel) {
		Control::_notification(p_what);
		return;
	}

	if (p_what == NOTIFICATION_DRAW) {
		panel->_draw_grid(this);
	} else if (p_what == NOTIFICATION_RESIZED) {
		panel->_on_canvas_resized();
	}
	Control::_notification(p_what);
}

void NvimGridCanvas::_gui_input(const Ref<InputEvent> &p_event) {
	if (panel && panel->_handle_gui_input(p_event)) {
		accept_event();
	} else {
		Control::_gui_input(p_event);
	}
}

NvimPanel::NvimPanel() {
	font_path_setting = _get_default_font_path();
	autostart = true;
	extra_args_setting = PackedStringArray();
	nvim_client = std::make_unique<NvimClient>();
	_ensure_grid(current_grid_id, grid_columns, grid_rows);
	_reset_highlight_defaults();
}

NvimPanel::~NvimPanel() {
	stop_nvim();
}

void NvimPanel::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_nvim"), &NvimPanel::start_nvim);
	ClassDB::bind_method(D_METHOD("stop_nvim"), &NvimPanel::stop_nvim);
	ClassDB::bind_method(D_METHOD("is_running"), &NvimPanel::is_running);
	ClassDB::bind_method(D_METHOD("set_nvim_command", "command"), &NvimPanel::set_nvim_command);
	ClassDB::bind_method(D_METHOD("get_nvim_command"), &NvimPanel::get_nvim_command);
	ClassDB::bind_method(D_METHOD("send_input", "keys"), &NvimPanel::send_input);
	ClassDB::bind_method(D_METHOD("send_command", "command"), &NvimPanel::send_command);
	ClassDB::bind_method(D_METHOD("open_file_in_nvim", "path"), &NvimPanel::open_file_in_nvim);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "nvim_command"), "set_nvim_command", "get_nvim_command");
}

void NvimPanel::_ready() {
	set_anchors_preset(PRESET_FULL_RECT);
	set_process(true);
	_ensure_ui_created();
	reload_settings();
	_update_ui_state();
	if (autostart && !is_running()) {
		call_deferred("start_nvim");
	}
}

void NvimPanel::_exit_tree() {
	stop_nvim();
}

void NvimPanel::_process(double p_delta) {
	(void)p_delta;
	_poll_nvim();
}

void NvimPanel::_ensure_ui_created() {
	if (root) {
		return;
	}

	root = memnew(VBoxContainer);
	root->set_anchors_preset(PRESET_FULL_RECT);
	root->set_h_size_flags(SIZE_FILL | SIZE_EXPAND);
	root->set_v_size_flags(SIZE_FILL | SIZE_EXPAND);
	add_child(root);

	grid_canvas = memnew(NvimGridCanvas);
	grid_canvas->set_panel(this);
	grid_canvas->set_h_size_flags(SIZE_FILL | SIZE_EXPAND);
	grid_canvas->set_v_size_flags(SIZE_FILL | SIZE_EXPAND);
	grid_canvas->set_focus_mode(FOCUS_ALL);
	root->add_child(grid_canvas);

	status_overlay = memnew(CenterContainer);
	status_overlay->set_h_size_flags(SIZE_FILL | SIZE_EXPAND);
	status_overlay->set_v_size_flags(SIZE_FILL | SIZE_EXPAND);
	status_overlay->set_visible(false);
	root->add_child(status_overlay);

	VBoxContainer *status_box = memnew(VBoxContainer);
	status_box->set_alignment(BoxContainer::ALIGNMENT_CENTER);
	status_box->set_h_size_flags(SIZE_SHRINK_CENTER);
	status_box->set_v_size_flags(SIZE_SHRINK_CENTER);
	status_overlay->add_child(status_box);

	status_label = memnew(Label);
	status_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
	status_label->set_text("Neovim is not running.");
	status_box->add_child(status_label);

	status_button = memnew(Button);
	status_button->set_text("Start Neovim");
	status_button->set_h_size_flags(SIZE_SHRINK_CENTER);
	status_box->add_child(status_button);
	status_button->connect("pressed", callable_mp(this, &NvimPanel::_on_status_button_pressed));

	_update_canvas_size();
	_request_grid_redraw();
}

void NvimPanel::set_nvim_command(const String &p_command) {
	nvim_command = p_command;
}

String NvimPanel::get_nvim_command() const {
	return nvim_command;
}

bool NvimPanel::is_running() {
	return nvim_client && nvim_client->is_running();
}

void NvimPanel::start_nvim() {
	reload_settings();
	if (is_running()) {
		if (debug_logging_enabled) {
			UtilityFunctions::print("[nvim_embed] Neovim is already running (pid = ", nvim_pid, ")");
		}
		return;
	}

	nvim_crashed = false;

	grids.clear();
	_ensure_grid(current_grid_id, grid_columns, grid_rows);
	highlight_definitions.clear();
	_apply_theme_defaults(true);

	if (!nvim_client) {
		nvim_client = std::make_unique<NvimClient>();
	}

	stdout_buffer.clear();

	CharString cmd_utf8 = nvim_command.utf8();
	std::string command(cmd_utf8.get_data());
	std::vector<std::string> args;
	args.emplace_back("--embed");
	for (int i = 0; i < extra_args_setting.size(); ++i) {
		CharString extra_utf8 = extra_args_setting[i].utf8();
		if (extra_utf8.length() > 0) {
			args.emplace_back(extra_utf8.get_data());
		}
	}
	if (!theme_colorscheme_name.is_empty()) {
		CharString theme_utf8 = theme_colorscheme_name.utf8();
		if (theme_utf8.length() > 0) {
			std::string theme_arg = "+colorscheme ";
			theme_arg += theme_utf8.get_data();
			args.emplace_back(theme_arg);
		}
	}

	std::string working_dir;
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (project_settings) {
		String project_path = project_settings->globalize_path("res://");
		CharString project_path_utf8 = project_path.utf8();
		working_dir = project_path_utf8.get_data();
	}

	if (!nvim_client->start(command, args, working_dir)) {
		UtilityFunctions::printerr("[nvim_embed] Failed to start Neovim process using command: ", nvim_command);
		nvim_pid = INVALID_PID;
		return;
	}

	nvim_pid = static_cast<int64_t>(nvim_client->get_pid());
	if (debug_logging_enabled) {
		UtilityFunctions::print("[nvim_embed] Launched Neovim process (pid = ", nvim_pid, ")");
	}
	_update_ui_state();

	_send_ui_attach();

	if (grid_canvas) {
		grid_canvas->grab_focus();
	}
}

void NvimPanel::stop_nvim() {
	if (!nvim_client) {
		return;
	}

	if (nvim_client->is_running()) {
		nvim_client->stop();
	}

	nvim_pid = INVALID_PID;
	stdout_buffer.clear();
	grids.clear();
	_ensure_grid(current_grid_id, grid_columns, grid_rows);
	highlight_definitions.clear();
	_apply_theme_defaults(true);
	nvim_crashed = false;
	_update_ui_state();
}

bool NvimPanel::send_input(const String &p_keys) {
	return _send_nvim_input(p_keys);
}

bool NvimPanel::send_command(const String &p_command) {
	return _send_nvim_command(p_command);
}

bool NvimPanel::open_file_in_nvim(const String &p_path) {
	if (!nvim_client || !nvim_client->is_running()) {
		return false;
	}

	if (p_path.is_empty()) {
		return false;
	}

	String quoted_path = _quote_path_for_command(p_path);
	return send_command("edit " + quoted_path);
}

void NvimPanel::_update_ui_state() {
	const bool running = is_running();

	if (grid_canvas) {
		grid_canvas->set_visible(running);
	}

	if (status_overlay) {
		status_overlay->set_visible(!running);
		if (!running && status_label && status_button) {
			if (nvim_crashed) {
				status_label->set_text("Neovim process exited unexpectedly.");
				status_button->set_text("Restart Neovim");
				status_button->set_disabled(false);
			} else if (!autostart) {
				status_label->set_text("Neovim autostart is disabled.");
				status_button->set_text("Start Neovim");
				status_button->set_disabled(false);
			} else {
				status_label->set_text("Neovim is not running.");
				status_button->set_text("Start Neovim");
				status_button->set_disabled(false);
			}
		}
	}

	_update_canvas_size();
	_request_grid_redraw();
}

void NvimPanel::_poll_nvim() {
	if (!nvim_client) {
		return;
	}

	if (!nvim_client->is_running()) {
		if (nvim_pid != INVALID_PID) {
			if (debug_logging_enabled) {
				UtilityFunctions::print("[nvim_embed] Neovim process exited.");
			}
			nvim_crashed = true;
			nvim_pid = INVALID_PID;
			_update_ui_state();
			grids.clear();
			_ensure_grid(current_grid_id, grid_columns, grid_rows);
			highlight_definitions.clear();
			_apply_theme_defaults(true);
		}
		return;
	}

	std::vector<uint8_t> incoming = nvim_client->read_available();
	if (!incoming.empty()) {
		stdout_buffer.insert(stdout_buffer.end(), incoming.begin(), incoming.end());
		if (debug_logging_enabled) {
			UtilityFunctions::print("[nvim_embed] Received ", static_cast<int64_t>(incoming.size()), " bytes from Neovim");
		}
	}

	while (_try_process_message()) {
		// Keep processing buffered messages until we hit a partial one.
	}
}

bool NvimPanel::_try_process_message() {
	if (stdout_buffer.empty()) {
		return false;
	}

	mpack_tree_t tree;
	mpack_tree_init_data(&tree, reinterpret_cast<const char *>(stdout_buffer.data()), stdout_buffer.size());
	mpack_tree_parse(&tree);

	mpack_error_t tree_error = mpack_tree_error(&tree);
	if (tree_error == mpack_error_eof || tree_error == mpack_error_io) {
		mpack_tree_destroy(&tree);
		return false;
	}
	if (tree_error != mpack_ok) {
		const char *error_text = mpack_error_to_string(tree_error);
		String error_string = error_text ? String::utf8(error_text) : String();
		UtilityFunctions::printerr("[nvim_embed] Failed to parse MessagePack from Neovim (error ", static_cast<int64_t>(tree_error), ": ", error_string, ")");
		mpack_tree_destroy(&tree);
		if (tree_error == mpack_error_invalid || tree_error == mpack_error_memory || tree_error == mpack_error_bug || tree_error == mpack_error_unsupported) {
			stdout_buffer.clear();
		}
		return false;
	}

	mpack_node_t root = mpack_tree_root(&tree);
	if (mpack_node_type(root) != mpack_type_array) {
		mpack_tree_destroy(&tree);
		stdout_buffer.clear();
		UtilityFunctions::printerr("[nvim_embed] Unexpected root type in RPC message.");
		return false;
	}

	uint32_t outer_size = mpack_node_array_length(root);
	if (outer_size == 0) {
		mpack_tree_destroy(&tree);
		stdout_buffer.clear();
		UtilityFunctions::printerr("[nvim_embed] Empty RPC message received.");
		return false;
	}

	mpack_node_t type_node = mpack_node_array_at(root, 0);
	int32_t message_type = mpack_node_i32(type_node);

	switch (message_type) {
		case 0: // Request
			if (debug_logging_enabled) {
				UtilityFunctions::print("[nvim_embed] Ignoring RPC request from Neovim (not implemented yet).");
			}
			break;
		case 1: // Response
			// TODO: Track pending requests if we need results.
			break;
		case 2: { // Notification
			if (outer_size < 2) {
				break;
			}

			mpack_node_t method_node = mpack_node_array_at(root, 1);
			size_t method_len = mpack_node_strlen(method_node);
			String method = String::utf8(mpack_node_str(method_node), static_cast<int64_t>(method_len));

			if (method == "redraw" && outer_size >= 3) {
				mpack_node_t batches_node = mpack_node_array_at(root, 2);
				if (mpack_node_type(batches_node) == mpack_type_array) {
					_handle_redraw(batches_node);
				}
			} else {
				size_t param_count = (outer_size > 2) ? outer_size - 2 : 0;
				if (debug_logging_enabled) {
					UtilityFunctions::print("[nvim_embed] Notification: ", method, " (", static_cast<int64_t>(param_count), " params)");
				}
			}
			break;
		}
		default:
			UtilityFunctions::printerr("[nvim_embed] Unknown RPC message type from Neovim: ", message_type);
			break;
	}

	size_t consumed = mpack_tree_size(&tree);
	mpack_tree_destroy(&tree);

	if (consumed == 0 || consumed > stdout_buffer.size()) {
		stdout_buffer.clear();
		return false;
	}

	stdout_buffer.erase(stdout_buffer.begin(), stdout_buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
	return true;
}

void NvimPanel::_send_ui_attach() {
	if (!nvim_client || !nvim_client->is_running()) {
		return;
	}

	uint32_t request_id = next_request_id++;

	char *buffer = nullptr;
	size_t buffer_size = 0;
	mpack_writer_t writer;
	mpack_writer_init_growable(&writer, &buffer, &buffer_size);

	mpack_start_array(&writer, 4);
	mpack_write_i32(&writer, 0); // Request message type.
	mpack_write_u32(&writer, request_id);
	mpack_write_cstr(&writer, "nvim_ui_attach");

	mpack_start_array(&writer, 3);
	mpack_write_u32(&writer, static_cast<uint32_t>(std::max(grid_columns, 1)));
	mpack_write_u32(&writer, static_cast<uint32_t>(std::max(grid_rows, 1)));

	mpack_start_map(&writer, 4);
	mpack_write_cstr(&writer, "rgb");
	mpack_write_bool(&writer, true);
	mpack_write_cstr(&writer, "ext_linegrid");
	mpack_write_bool(&writer, true);
	mpack_write_cstr(&writer, "ext_hlstate");
	mpack_write_bool(&writer, true);
	mpack_write_cstr(&writer, "ext_termcolors");
	mpack_write_bool(&writer, true);
	mpack_finish_map(&writer);
	mpack_finish_array(&writer);
	mpack_finish_array(&writer);

	mpack_error_t writer_error = mpack_writer_destroy(&writer);
	if (writer_error != mpack_ok) {
		if (buffer) {
			std::free(buffer);
		}
		UtilityFunctions::printerr("[nvim_embed] Failed to encode nvim_ui_attach request (error code ", static_cast<int64_t>(writer_error), ")");
		return;
	}

	if (!buffer || buffer_size == 0) {
		UtilityFunctions::printerr("[nvim_embed] Generated empty nvim_ui_attach payload");
		if (buffer) {
			std::free(buffer);
		}
		return;
	}

	size_t written = nvim_client->write(reinterpret_cast<const uint8_t *>(buffer), buffer_size);
	std::free(buffer);

	if (written != buffer_size) {
		UtilityFunctions::printerr("[nvim_embed] Failed to write full nvim_ui_attach request (", static_cast<int64_t>(written), "/", static_cast<int64_t>(buffer_size), " bytes)");
		return;
	}

	if (debug_logging_enabled) {
		UtilityFunctions::print("[nvim_embed] Sent nvim_ui_attach request (#", static_cast<int64_t>(request_id), ")");
	}
}

void NvimPanel::_handle_redraw(const mpack_node_t &p_batches_node) {
	size_t batch_count = mpack_node_array_length(p_batches_node);
	int64_t logged_events = 0;
	for (size_t batch_index = 0; batch_index < batch_count; ++batch_index) {
		mpack_node_t event_node = mpack_node_array_at(p_batches_node, batch_index);
		if (mpack_node_type(event_node) != mpack_type_array || mpack_node_array_length(event_node) == 0) {
			continue;
		}

		mpack_node_t event_name_node = mpack_node_array_at(event_node, 0);
		size_t event_name_len = mpack_node_strlen(event_name_node);
		String event_name = String::utf8(mpack_node_str(event_name_node), static_cast<int64_t>(event_name_len));

		size_t part_count = mpack_node_array_length(event_node);
		for (size_t arg_index = 1; arg_index < part_count; ++arg_index) {
			mpack_node_t arg_node = mpack_node_array_at(event_node, arg_index);
			_handle_redraw_event(event_name, arg_node);
		}

		if (logged_events < 5) {
			size_t arg_count = (part_count > 0) ? part_count - 1 : 0;
			if (debug_logging_enabled) {
				UtilityFunctions::print("[nvim_embed] redraw/", event_name, " (", static_cast<int64_t>(arg_count), " args)");
			}
		}
		++logged_events;
	}
	if (debug_logging_enabled) {
		UtilityFunctions::print("[nvim_embed] redraw batch contained ", static_cast<int64_t>(batch_count), " events");
	}
}

void NvimPanel::_handle_redraw_event(const String &p_event_name, const mpack_node_t &p_args_node) {
	if (mpack_node_type(p_args_node) != mpack_type_array) {
		return;
	}

	if (p_event_name == "grid_resize") {
		_handle_grid_resize(p_args_node);
	} else if (p_event_name == "grid_clear") {
		_handle_grid_clear(p_args_node);
	} else if (p_event_name == "grid_destroy") {
		_handle_grid_destroy(p_args_node);
	} else if (p_event_name == "grid_line") {
		_handle_grid_line(p_args_node);
	} else if (p_event_name == "grid_cursor_goto") {
		_handle_grid_cursor_goto(p_args_node);
	} else if (p_event_name == "grid_scroll") {
		_handle_grid_scroll(p_args_node);
	} else if (p_event_name == "hl_attr_define") {
		_handle_hl_attr_define(p_args_node);
	} else if (p_event_name == "default_colors_set") {
		_handle_default_colors_set(p_args_node);
	}
}

void NvimPanel::_handle_grid_resize(const mpack_node_t &p_args_node) {
	size_t arg_len = mpack_node_array_length(p_args_node);
	if (arg_len < 3) {
		return;
	}

	int64_t grid_id = mpack_node_i64(mpack_node_array_at(p_args_node, 0));
	int64_t columns = mpack_node_i64(mpack_node_array_at(p_args_node, 1));
	int64_t rows = mpack_node_i64(mpack_node_array_at(p_args_node, 2));

	NvimGrid &grid = _ensure_grid(grid_id, static_cast<int32_t>(columns), static_cast<int32_t>(rows));
	for (int32_t row = 0; row < grid.rows; ++row) {
		_fill_row(grid.cells[row]);
	}

	if (grid_id == current_grid_id) {
		grid_columns = grid.columns;
		grid_rows = grid.rows;
	}

	_update_canvas_size();
	_request_grid_redraw();
}

void NvimPanel::_handle_grid_clear(const mpack_node_t &p_args_node) {
	size_t arg_len = mpack_node_array_length(p_args_node);
	if (arg_len < 1) {
		return;
	}

	int64_t grid_id = mpack_node_i64(mpack_node_array_at(p_args_node, 0));
	auto it = grids.find(grid_id);
	if (it == grids.end()) {
		return;
	}

	NvimGrid &grid = it->second;
	for (int32_t row = 0; row < grid.rows; ++row) {
		std::vector<NvimCell> &row_cells = grid.cells[row];
		_fill_row(row_cells);
	}

	_request_grid_redraw();
}

void NvimPanel::_handle_grid_destroy(const mpack_node_t &p_args_node) {
	size_t arg_len = mpack_node_array_length(p_args_node);
	if (arg_len < 1) {
		return;
	}

	int64_t grid_id = mpack_node_i64(mpack_node_array_at(p_args_node, 0));
	grids.erase(grid_id);
	if (grid_id == current_grid_id) {
		current_grid_id = 0;
		_ensure_grid(current_grid_id, grid_columns, grid_rows);
	}

	_update_canvas_size();
	_request_grid_redraw();
}

void NvimPanel::_handle_grid_line(const mpack_node_t &p_args_node) {
	size_t arg_len = mpack_node_array_length(p_args_node);
	if (arg_len < 4) {
		return;
	}

	int64_t grid_id = mpack_node_i64(mpack_node_array_at(p_args_node, 0));
	int64_t row = mpack_node_i64(mpack_node_array_at(p_args_node, 1));
	int64_t column = mpack_node_i64(mpack_node_array_at(p_args_node, 2));
	mpack_node_t cells_node = mpack_node_array_at(p_args_node, 3);
	if (mpack_node_type(cells_node) != mpack_type_array) {
		return;
	}

	NvimGrid &grid = _ensure_grid(grid_id, grid_columns, grid_rows);
	if (row < 0 || row >= grid.rows) {
		return;
	}

	std::vector<NvimCell> &row_cells = grid.cells[static_cast<size_t>(row)];
	int64_t write_column = column;
	int64_t last_hl_id = 0;
	size_t cell_count = mpack_node_array_length(cells_node);
	for (size_t cell_index = 0; cell_index < cell_count; ++cell_index) {
		mpack_node_t cell_entry = mpack_node_array_at(cells_node, cell_index);
		if (mpack_node_type(cell_entry) != mpack_type_array || mpack_node_array_length(cell_entry) == 0) {
			continue;
		}

		mpack_node_t text_node = mpack_node_array_at(cell_entry, 0);
		size_t text_len = mpack_node_strlen(text_node);
		String text = String::utf8(mpack_node_str(text_node), static_cast<int64_t>(text_len));
		if (text.is_empty()) {
			text = " ";
		}

		int64_t hl_id = last_hl_id;
		if (mpack_node_array_length(cell_entry) >= 2) {
			mpack_node_t hl_node = mpack_node_array_at(cell_entry, 1);
			if (mpack_node_type(hl_node) != mpack_type_nil) {
				hl_id = mpack_node_i64(hl_node);
				last_hl_id = hl_id;
			}
		}

		int64_t repeat = 1;
		if (mpack_node_array_length(cell_entry) >= 3) {
			repeat = std::max<int64_t>(1, mpack_node_i64(mpack_node_array_at(cell_entry, 2)));
		}

		for (int64_t r = 0; r < repeat && write_column < grid.columns; ++r, ++write_column) {
			NvimCell &cell = row_cells[static_cast<size_t>(write_column)];
			cell.text = text;
			cell.hl_id = hl_id;
		}
	}

	_request_grid_redraw();
}

void NvimPanel::_handle_grid_cursor_goto(const mpack_node_t &p_args_node) {
	size_t arg_len = mpack_node_array_length(p_args_node);
	if (arg_len < 3) {
		return;
	}

	current_grid_id = mpack_node_i64(mpack_node_array_at(p_args_node, 0));
	cursor_row = mpack_node_i64(mpack_node_array_at(p_args_node, 1));
	cursor_column = mpack_node_i64(mpack_node_array_at(p_args_node, 2));
	_ensure_grid(current_grid_id, grid_columns, grid_rows);
	_request_grid_redraw();
}

void NvimPanel::_handle_grid_scroll(const mpack_node_t &p_args_node) {
	size_t arg_len = mpack_node_array_length(p_args_node);
	if (arg_len < 7) {
		return;
	}

	int64_t grid_id = mpack_node_i64(mpack_node_array_at(p_args_node, 0));
	int64_t top = mpack_node_i64(mpack_node_array_at(p_args_node, 1));
	int64_t bottom = mpack_node_i64(mpack_node_array_at(p_args_node, 2));
	int64_t left = mpack_node_i64(mpack_node_array_at(p_args_node, 3));
	int64_t right = mpack_node_i64(mpack_node_array_at(p_args_node, 4));
	int64_t rows = mpack_node_i64(mpack_node_array_at(p_args_node, 5));
	int64_t cols = mpack_node_i64(mpack_node_array_at(p_args_node, 6));

	NvimGrid &grid = _ensure_grid(grid_id, grid_columns, grid_rows);
	int64_t height = bottom - top;
	int64_t width = right - left;
	if (height <= 0 || width <= 0) {
		return;
	}

	std::vector<std::vector<NvimCell>> region(static_cast<size_t>(height), std::vector<NvimCell>(static_cast<size_t>(width)));
	for (int64_t r = 0; r < height; ++r) {
		int64_t grid_row = top + r;
		if (grid_row < 0 || grid_row >= grid.rows) {
			continue;
		}
		for (int64_t c = 0; c < width; ++c) {
			int64_t grid_col = left + c;
			if (grid_col < 0 || grid_col >= grid.columns) {
				continue;
			}
			region[static_cast<size_t>(r)][static_cast<size_t>(c)] = grid.cells[static_cast<size_t>(grid_row)][static_cast<size_t>(grid_col)];
		}
	}

	for (int64_t r = 0; r < height; ++r) {
		int64_t grid_row = top + r;
		if (grid_row < 0 || grid_row >= grid.rows) {
			continue;
		}
		for (int64_t c = 0; c < width; ++c) {
			int64_t grid_col = left + c;
			if (grid_col < 0 || grid_col >= grid.columns) {
				continue;
			}

			int64_t src_r = r + rows;
			int64_t src_c = c + cols;
			NvimCell cell;
			if (src_r >= 0 && src_r < height && src_c >= 0 && src_c < width) {
				cell = region[static_cast<size_t>(src_r)][static_cast<size_t>(src_c)];
			} else {
				cell.text = " ";
				cell.hl_id = 0;
			}

			grid.cells[static_cast<size_t>(grid_row)][static_cast<size_t>(grid_col)] = cell;
		}
	}

	_request_grid_redraw();
}

NvimPanel::NvimGrid &NvimPanel::_ensure_grid(int64_t p_grid_id, int32_t p_columns, int32_t p_rows) {
	NvimGrid &grid = grids[p_grid_id];
	if (p_columns <= 0) {
		p_columns = grid.columns > 0 ? grid.columns : grid_columns;
	}
	if (p_rows <= 0) {
		p_rows = grid.rows > 0 ? grid.rows : grid_rows;
	}

	if (grid.columns != p_columns || grid.rows != p_rows || grid.cells.empty()) {
		grid.columns = p_columns;
		grid.rows = p_rows;
		grid.cells.resize(static_cast<size_t>(grid.rows));
		for (int32_t row = 0; row < grid.rows; ++row) {
			std::vector<NvimCell> &row_cells = grid.cells[static_cast<size_t>(row)];
			row_cells.resize(static_cast<size_t>(grid.columns));
			_fill_row(row_cells);
		}
	}

	return grid;
}

void NvimPanel::_fill_row(std::vector<NvimCell> &p_row) {
	for (NvimCell &cell : p_row) {
		cell.text = " ";
		cell.hl_id = 0;
	}
}

void NvimPanel::_handle_hl_attr_define(const mpack_node_t &p_args_node) {
	if (mpack_node_type(p_args_node) != mpack_type_array || mpack_node_array_length(p_args_node) < 2) {
		return;
	}

	int64_t hl_id = mpack_node_i64(mpack_node_array_at(p_args_node, 0));
	Highlight &highlight = highlight_definitions[hl_id];

	mpack_node_t rgb_attrs = mpack_node_array_at(p_args_node, 1);
	if (mpack_node_type(rgb_attrs) == mpack_type_map) {
		size_t map_count = mpack_node_map_count(rgb_attrs);
		bool reverse = false;
		Color fg = highlight.foreground;
		Color bg = highlight.background;
		bool fg_set = highlight.has_foreground;
		bool bg_set = highlight.has_background;

		for (size_t i = 0; i < map_count; ++i) {
			mpack_node_t key_node = mpack_node_map_key_at(rgb_attrs, i);
			mpack_node_t value_node = mpack_node_map_value_at(rgb_attrs, i);
			size_t key_len = mpack_node_strlen(key_node);
			String key = String::utf8(mpack_node_str(key_node), static_cast<int64_t>(key_len));

			mpack_type_t value_type = mpack_node_type(value_node);
			if ((key == "foreground" || key == "special") && (value_type == mpack_type_uint || value_type == mpack_type_int)) {
				int64_t color_value = mpack_node_i64(value_node);
				fg = _color_from_rgb_value(color_value);
				fg_set = true;
			} else if (key == "background" && (value_type == mpack_type_uint || value_type == mpack_type_int)) {
				int64_t color_value = mpack_node_i64(value_node);
				bg = _color_from_rgb_value(color_value);
				bg_set = true;
			} else if (key == "reverse" && value_type == mpack_type_bool) {
				reverse = mpack_node_bool(value_node);
			}
		}

		if (reverse) {
			Color tmp_fg = fg;
			fg = bg;
			bg = tmp_fg;
			bool tmp_fg_set = fg_set;
			fg_set = bg_set;
			bg_set = tmp_fg_set;
		}

		highlight.foreground = fg;
		highlight.background = bg;
		highlight.has_foreground = fg_set;
		highlight.has_background = bg_set;
	}

	_request_grid_redraw();
}

void NvimPanel::_handle_default_colors_set(const mpack_node_t &p_args_node) {
	if (mpack_node_type(p_args_node) != mpack_type_array || mpack_node_array_length(p_args_node) < 1) {
		return;
	}

	mpack_node_t attrs_node = mpack_node_array_at(p_args_node, 0);
	if (mpack_node_type(attrs_node) != mpack_type_map) {
		return;
	}

	size_t map_count = mpack_node_map_count(attrs_node);
	for (size_t i = 0; i < map_count; ++i) {
		mpack_node_t key_node = mpack_node_map_key_at(attrs_node, i);
		mpack_node_t value_node = mpack_node_map_value_at(attrs_node, i);
		size_t key_len = mpack_node_strlen(key_node);
		String key = String::utf8(mpack_node_str(key_node), static_cast<int64_t>(key_len));
		mpack_type_t value_type = mpack_node_type(value_node);
		if ((value_type == mpack_type_uint || value_type == mpack_type_int)) {
			int64_t color_value = mpack_node_i64(value_node);
			if (key == "foreground") {
				default_foreground = _color_from_rgb_value(color_value);
			} else if (key == "background") {
				default_background = _color_from_rgb_value(color_value);
			}
		}
	}

	_request_grid_redraw();
}

Color NvimPanel::_color_from_rgb_value(int64_t p_value) const {
	if (p_value < 0) {
		return Color(0, 0, 0, 0);
	}
	uint32_t rgb = static_cast<uint32_t>(p_value);
	float r = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
	float g = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
	float b = static_cast<float>(rgb & 0xFF) / 255.0f;
	return Color(r, g, b, 1.0f);
}

Color NvimPanel::_resolve_foreground(int64_t p_hl_id) const {
	auto it = highlight_definitions.find(p_hl_id);
	if (it != highlight_definitions.end() && it->second.has_foreground) {
		return it->second.foreground;
	}
	return default_foreground;
}

Color NvimPanel::_resolve_background(int64_t p_hl_id) const {
	auto it = highlight_definitions.find(p_hl_id);
	if (it != highlight_definitions.end() && it->second.has_background) {
		return it->second.background;
	}
	return default_background;
}

Ref<Font> NvimPanel::_obtain_font() const {
	if (cached_font.is_valid()) {
		return cached_font;
	}

	ResourceLoader *loader = ResourceLoader::get_singleton();
	String font_path = font_path_setting.is_empty() ? _get_default_font_path() : font_path_setting;
	if (loader && !font_path.is_empty()) {
		Ref<Resource> font_res = loader->load(font_path);
		Ref<Font> custom_font = font_res;
		if (custom_font.is_valid()) {
			cached_font = custom_font;
			return cached_font;
		}
	}

	ThemeDB *theme_db = ThemeDB::get_singleton();
	Ref<Theme> theme;
	if (theme_db) {
		theme = theme_db->get_project_theme();
		if (!theme.is_valid()) {
			theme = theme_db->get_default_theme();
		}
	}

	if (theme.is_valid()) {
		Ref<Font> theme_font = theme->get_font(StringName("font"), StringName("Label"));
		if (theme_font.is_valid()) {
			cached_font = theme_font;
			return cached_font;
		}
	}

	if (theme_db) {
		Ref<Font> fallback_font = theme_db->get_fallback_font();
		if (fallback_font.is_valid()) {
			cached_font = fallback_font;
			return cached_font;
		}
	}

	return Ref<Font>();
}

int32_t NvimPanel::_obtain_font_size() const {
	ThemeDB *theme_db = ThemeDB::get_singleton();
	Ref<Theme> theme;
	if (theme_db) {
		theme = theme_db->get_project_theme();
		if (!theme.is_valid()) {
			theme = theme_db->get_default_theme();
		}
	}

	if (theme.is_valid()) {
		int32_t theme_size = theme->get_font_size(StringName("font_size"), StringName("Label"));
		if (theme_size > 0) {
			return theme_size;
		}
	}

	if (theme_db) {
		int32_t fallback_size = theme_db->get_fallback_font_size();
		if (fallback_size > 0) {
			return fallback_size;
		}
	}

	return font_size;
}

void NvimPanel::_draw_grid(NvimGridCanvas *p_canvas) {
	if (!p_canvas) {
		return;
	}

	auto grid_it = grids.find(current_grid_id);
	if (grid_it == grids.end()) {
		if (grids.empty()) {
			return;
		}
		grid_it = grids.begin();
	}

	const NvimGrid &grid = grid_it->second;
	if (grid.columns <= 0 || grid.rows <= 0) {
		return;
	}

	Ref<Font> font = _obtain_font();
	if (font.is_null()) {
		return;
	}

	int32_t size = _obtain_font_size();
	Vector2 char_size = font->get_char_size(U'M', size);
	float cell_w = char_size.x;
	if (cell_w <= 0) {
		char_size = font->get_char_size(U' ', size);
		cell_w = char_size.x;
	}
	if (cell_w <= 0) {
		cell_w = font->get_height(size) * 0.6f;
	}
	float cell_h = font->get_height(size);
	float ascent = font->get_ascent(size);
	cell_width = cell_w > 0 ? cell_w : 1.0f;
	cell_height = cell_h > 0 ? cell_h : 1.0f;
	cell_ascent = ascent >= 0 ? ascent : cell_height * 0.8f;

	Vector2 canvas_size = p_canvas->get_size();
	p_canvas->draw_rect(Rect2(Vector2(), canvas_size), default_background, true);

	for (int32_t row = 0; row < grid.rows; ++row) {
		if (static_cast<size_t>(row) >= grid.cells.size()) {
			break;
		}
		const std::vector<NvimCell> &row_cells = grid.cells[static_cast<size_t>(row)];
		for (int32_t col = 0; col < grid.columns && static_cast<size_t>(col) < row_cells.size(); ++col) {
			const NvimCell &cell = row_cells[static_cast<size_t>(col)];
			Vector2 cell_position(static_cast<float>(col) * cell_w, static_cast<float>(row) * cell_h);

			Color bg = _resolve_background(cell.hl_id);
			if (grid_it->first == current_grid_id && row == cursor_row && col == cursor_column) {
				bg = bg.lightened(0.3f);
			}
			if (bg.a > 0.0f) {
				p_canvas->draw_rect(Rect2(cell_position, Vector2(cell_w, cell_h)), bg, true);
			}

			Color fg = _resolve_foreground(cell.hl_id);
			Vector2 text_position(cell_position.x, cell_position.y + cell_ascent);
			p_canvas->draw_string(font, text_position, cell.text, HORIZONTAL_ALIGNMENT_LEFT, -1.0, size, fg);
		}
	}

	_sync_neovim_size_to_canvas();
}

void NvimPanel::_update_canvas_size() {
	if (!grid_canvas) {
		return;
	}

	// Allow the canvas to shrink with the editor; rely on the layout and Neovim resize
	// logic to adjust the grid rather than enforcing a large minimum size.
	grid_canvas->set_custom_minimum_size(Vector2());
}

void NvimPanel::_request_grid_redraw() {
	if (grid_canvas) {
		grid_canvas->queue_redraw();
	}
}

bool NvimPanel::_handle_gui_input(const Ref<InputEvent> &p_event) {
	if (!p_event.is_valid()) {
		return false;
	}

	if (!nvim_client || !nvim_client->is_running()) {
		return false;
	}

	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid()) {
		return _handle_key_event(key_event);
	}

	Ref<InputEventMouseButton> mouse_button_event = p_event;
	if (mouse_button_event.is_valid()) {
		return _handle_mouse_button_event(mouse_button_event);
	}

	Ref<InputEventMouseMotion> mouse_motion_event = p_event;
	if (mouse_motion_event.is_valid()) {
		return _handle_mouse_motion_event(mouse_motion_event);
	}

	return false;
}

bool NvimPanel::_handle_key_event(const Ref<InputEventKey> &p_key_event) {
	if (p_key_event.is_null()) {
		return false;
	}

	if (!p_key_event->is_pressed()) {
		return false;
	}

	String translated = _translate_key_event(p_key_event);
	if (translated.is_empty()) {
		return false;
	}

	if (_send_nvim_input(translated)) {
		if (grid_canvas) {
			grid_canvas->grab_focus();
		}
		return true;
	}

	return false;
}

bool NvimPanel::_handle_mouse_button_event(const Ref<InputEventMouseButton> &p_mouse_event) {
	if (p_mouse_event.is_null()) {
		return false;
	}

	if (grid_canvas) {
		grid_canvas->grab_focus();
	}

	MouseButton button_index = static_cast<MouseButton>(p_mouse_event->get_button_index());
	bool pressed = p_mouse_event->is_pressed();

	if (button_index == MouseButton::MOUSE_BUTTON_WHEEL_UP || button_index == MouseButton::MOUSE_BUTTON_WHEEL_DOWN || button_index == MouseButton::MOUSE_BUTTON_WHEEL_LEFT || button_index == MouseButton::MOUSE_BUTTON_WHEEL_RIGHT) {
		if (!pressed) {
			return false;
		}

		String base;
		switch (button_index) {
			case MouseButton::MOUSE_BUTTON_WHEEL_UP:
				base = "ScrollWheelUp";
				break;
			case MouseButton::MOUSE_BUTTON_WHEEL_DOWN:
				base = "ScrollWheelDown";
				break;
			case MouseButton::MOUSE_BUTTON_WHEEL_LEFT:
				base = "ScrollWheelLeft";
				break;
			case MouseButton::MOUSE_BUTTON_WHEEL_RIGHT:
				base = "ScrollWheelRight";
				break;
			default:
				return false;
		}

		String seq = _format_special_key(base, p_mouse_event->is_shift_pressed(), p_mouse_event->is_ctrl_pressed(), p_mouse_event->is_alt_pressed());
		return _send_nvim_input(seq);
	}

	String button_name;
	switch (button_index) {
		case MouseButton::MOUSE_BUTTON_LEFT:
			button_name = "left";
			break;
		case MouseButton::MOUSE_BUTTON_RIGHT:
			button_name = "right";
			break;
		case MouseButton::MOUSE_BUTTON_MIDDLE:
			button_name = "middle";
			break;
		default:
			return false;
	}

	String action = pressed ? "press" : "release";
	int64_t row = 0;
	int64_t column = 0;
	_convert_position_to_cell(p_mouse_event->get_position(), row, column);
	String modifiers = _build_modifier_string(p_mouse_event->is_shift_pressed(), p_mouse_event->is_ctrl_pressed(), p_mouse_event->is_alt_pressed());

	return _send_nvim_input_mouse(button_name, action, modifiers, current_grid_id, row, column);
}

bool NvimPanel::_handle_mouse_motion_event(const Ref<InputEventMouseMotion> &p_motion_event) {
	if (p_motion_event.is_null()) {
		return false;
	}

	int32_t mask = p_motion_event->get_button_mask();
	String button_name;
	if (mask & static_cast<int32_t>(MouseButtonMask::MOUSE_BUTTON_MASK_LEFT)) {
		button_name = "left";
	} else if (mask & static_cast<int32_t>(MouseButtonMask::MOUSE_BUTTON_MASK_RIGHT)) {
		button_name = "right";
	} else if (mask & static_cast<int32_t>(MouseButtonMask::MOUSE_BUTTON_MASK_MIDDLE)) {
		button_name = "middle";
	} else {
		return false;
	}

	int64_t row = 0;
	int64_t column = 0;
	_convert_position_to_cell(p_motion_event->get_position(), row, column);
	String modifiers = _build_modifier_string(p_motion_event->is_shift_pressed(), p_motion_event->is_ctrl_pressed(), p_motion_event->is_alt_pressed());

	return _send_nvim_input_mouse(button_name, "drag", modifiers, current_grid_id, row, column);
}

String NvimPanel::_translate_key_event(const Ref<InputEventKey> &p_key_event) const {
	Key keycode = static_cast<Key>(p_key_event->get_keycode());
	bool shift = p_key_event->is_shift_pressed();
	bool ctrl = p_key_event->is_ctrl_pressed();
	bool alt = p_key_event->is_alt_pressed();
	char32_t unicode = p_key_event->get_unicode();

	switch (keycode) {
		case Key::KEY_ENTER:
		case Key::KEY_KP_ENTER:
			return _format_special_key("CR", shift, ctrl, alt);
		case Key::KEY_TAB:
			return _format_special_key("Tab", shift, ctrl, alt);
		case Key::KEY_BACKTAB:
			return _format_special_key("Tab", true, ctrl, alt);
		case Key::KEY_ESCAPE:
			return _format_special_key("Esc", shift, ctrl, alt);
		case Key::KEY_BACKSPACE:
			return _format_special_key("BS", shift, ctrl, alt);
		case Key::KEY_SPACE:
			if (ctrl || alt || shift) {
				return _format_special_key("Space", shift, ctrl, alt);
			}
			return String(" ");
		case Key::KEY_UP:
			return _format_special_key("Up", shift, ctrl, alt);
		case Key::KEY_DOWN:
			return _format_special_key("Down", shift, ctrl, alt);
		case Key::KEY_LEFT:
			return _format_special_key("Left", shift, ctrl, alt);
		case Key::KEY_RIGHT:
			return _format_special_key("Right", shift, ctrl, alt);
		case Key::KEY_HOME:
			return _format_special_key("Home", shift, ctrl, alt);
		case Key::KEY_END:
			return _format_special_key("End", shift, ctrl, alt);
		case Key::KEY_PAGEUP:
			return _format_special_key("PageUp", shift, ctrl, alt);
		case Key::KEY_PAGEDOWN:
			return _format_special_key("PageDown", shift, ctrl, alt);
		case Key::KEY_INSERT:
			return _format_special_key("Insert", shift, ctrl, alt);
		case Key::KEY_DELETE:
			return _format_special_key("Del", shift, ctrl, alt);
		default:
			break;
	}

	if (keycode >= Key::KEY_F1 && keycode <= Key::KEY_F24) {
		int fn = static_cast<int>(keycode) - static_cast<int>(Key::KEY_F1) + 1;
		return _format_special_key(String("F") + String::num_int64(fn), shift, ctrl, alt);
	}

	char32_t base_char = unicode;
	if (base_char == 0) {
		if (keycode >= Key::KEY_A && keycode <= Key::KEY_Z) {
			int offset = static_cast<int>(keycode) - static_cast<int>(Key::KEY_A);
			base_char = shift ? static_cast<char32_t>('A' + offset) : static_cast<char32_t>('a' + offset);
		} else if (keycode >= Key::KEY_0 && keycode <= Key::KEY_9) {
			int offset = static_cast<int>(keycode) - static_cast<int>(Key::KEY_0);
			base_char = static_cast<char32_t>('0' + offset);
		}
	}

	if (base_char >= 32 && base_char != 127) {
		String text = String::chr(base_char);
		if (ctrl || alt) {
			String key_name = text;
			if (key_name.length() == 1 && key_name[0] >= 'a' && key_name[0] <= 'z') {
				key_name = key_name.to_upper();
			}
			return _format_special_key(key_name, shift, ctrl, alt);
		}
		return text;
	}

	return String();
}

String NvimPanel::_format_special_key(const String &p_key_name, bool p_shift, bool p_ctrl, bool p_alt) const {
	PackedStringArray modifiers;
	if (p_ctrl) {
		modifiers.push_back("C");
	}
	if (p_shift) {
		modifiers.push_back("S");
	}
	if (p_alt) {
		modifiers.push_back("A");
	}

	String inside;
	for (int i = 0; i < modifiers.size(); ++i) {
		inside += modifiers[i];
		inside += "-";
	}
	inside += p_key_name;
	return "<" + inside + ">";
}

String NvimPanel::_build_modifier_string(bool p_shift, bool p_ctrl, bool p_alt) const {
	String modifiers;
	if (p_ctrl) {
		modifiers += "C";
	}
	if (p_shift) {
		modifiers += "S";
	}
	if (p_alt) {
		modifiers += "A";
	}
	return modifiers;
}

void NvimPanel::_convert_position_to_cell(const Vector2 &p_position, int64_t &r_row, int64_t &r_column) const {
	float cw = cell_width > 0.0f ? cell_width : 1.0f;
	float ch = cell_height > 0.0f ? cell_height : 1.0f;
	int64_t max_col = grid_columns > 0 ? grid_columns - 1 : 0;
	int64_t max_row = grid_rows > 0 ? grid_rows - 1 : 0;

	int64_t column = static_cast<int64_t>(p_position.x / cw);
	int64_t row = static_cast<int64_t>(p_position.y / ch);
	if (column < 0) {
		column = 0;
	}
	if (row < 0) {
		row = 0;
	}
	if (column > max_col) {
		column = max_col;
	}
	if (row > max_row) {
		row = max_row;
	}

	r_column = column;
	r_row = row;
}

void NvimPanel::_sync_neovim_size_to_canvas() {
	if (!grid_canvas || !nvim_client || !nvim_client->is_running()) {
		return;
	}

	_ensure_cell_metrics();
	if (cell_width <= 0.0f || cell_height <= 0.0f) {
		return;
	}

	Vector2 canvas_size = grid_canvas->get_size();
	if (canvas_size.x <= 0.0f || canvas_size.y <= 0.0f) {
		return;
	}

	int32_t new_columns = Math::max(1, static_cast<int32_t>(canvas_size.x / cell_width));
	int32_t new_rows = Math::max(1, static_cast<int32_t>(canvas_size.y / cell_height));

	if (new_columns != grid_columns || new_rows != grid_rows) {
		if (_send_ui_try_resize(new_columns, new_rows)) {
			grid_columns = new_columns;
			grid_rows = new_rows;
			_ensure_grid(current_grid_id, grid_columns, grid_rows);
			_update_canvas_size();
		}
	}
}

bool NvimPanel::_send_ui_try_resize(int32_t p_columns, int32_t p_rows) {
	if (!nvim_client || !nvim_client->is_running()) {
		return false;
	}

	char *buffer = nullptr;
	size_t buffer_size = 0;
	mpack_writer_t writer;
	mpack_writer_init_growable(&writer, &buffer, &buffer_size);

	uint32_t request_id = next_request_id++;
	mpack_start_array(&writer, 4);
	mpack_write_i32(&writer, 0);
	mpack_write_u32(&writer, request_id);
	mpack_write_cstr(&writer, "nvim_ui_try_resize");
	mpack_start_array(&writer, 2);
	mpack_write_i32(&writer, p_columns);
	mpack_write_i32(&writer, p_rows);
	mpack_finish_array(&writer);
	mpack_finish_array(&writer);

	mpack_error_t writer_error = mpack_writer_destroy(&writer);
	if (writer_error != mpack_ok || !buffer || buffer_size == 0) {
		if (buffer) {
			std::free(buffer);
		}
		return false;
	}

	size_t written = nvim_client->write(reinterpret_cast<const uint8_t *>(buffer), buffer_size);
	std::free(buffer);
	return written == buffer_size;
}

void NvimPanel::_on_status_button_pressed() {
	start_nvim();
}

String NvimPanel::_quote_path_for_command(const String &p_path) const {
	if (p_path.is_empty()) {
		return p_path;
	}

	String escaped = p_path;
	escaped = escaped.replace("'", "''");
	return "'" + escaped + "'";
}

void NvimPanel::_reset_highlight_defaults() {
	Highlight base;
	base.foreground = theme_default_foreground;
	base.background = theme_default_background;
	base.has_foreground = true;
	base.has_background = true;
	highlight_definitions[0] = base;
}

void NvimPanel::_apply_theme_defaults(bool p_update_immediately) {
	default_foreground = theme_default_foreground;
	default_background = theme_default_background;
	if (p_update_immediately) {
		_reset_highlight_defaults();
		_request_grid_redraw();
	}
}

bool NvimPanel::_load_theme_definition(const String &p_theme_name) {
	String sanitized = p_theme_name.strip_edges();
	if (sanitized.is_empty()) {
		sanitized = "default";
	}
	String original_case_name = sanitized;
	String reduced = sanitized.to_lower();
	reduced = reduced.replace(" ", "_");
	if (reduced.is_empty()) {
		reduced = "default";
	}

	const String base_path = "res://addons/VimDot/themes/";
	PackedStringArray candidates;
	candidates.push_back(base_path + reduced + ".theme");
	if (reduced != "default") {
		candidates.push_back(base_path + String("default.theme"));
	}

	bool loaded = false;
	String loaded_theme_name = reduced;
	theme_colorscheme_name = String();

	for (int64_t i = 0; i < candidates.size(); ++i) {
		const String &path = candidates[i];
		if (!FileAccess::file_exists(path)) {
			continue;
		}

		Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
		if (file.is_null()) {
			UtilityFunctions::printerr("[nvim_embed] Failed to open theme file: ", path);
			continue;
		}

		Color fg = theme_default_foreground;
		Color bg = theme_default_background;
		bool fg_set = false;
		bool bg_set = false;
		String colorscheme_name;

		while (!file->eof_reached()) {
			String line = file->get_line().strip_edges();
			if (line.is_empty() || line.begins_with("#")) {
				continue;
			}

			int64_t equals_index = line.find("=");
			if (equals_index < 0) {
				continue;
			}

			String key = line.substr(0, equals_index).strip_edges().to_lower();
			key = key.replace("-", "_");
			String value = line.substr(equals_index + 1).strip_edges();
			if (value.is_empty()) {
				continue;
			}

			if (key == "default_foreground" || key == "default_font_color" || key == "foreground" || key == "font_color" || key == "text_color") {
				bool valid = false;
				Color parsed = _parse_theme_color(value, valid);
				if (valid) {
					fg = parsed;
					fg_set = true;
				}
				else {
					UtilityFunctions::printerr("[nvim_embed] Theme color '", value, "' for key '", key, "' is invalid in ", path);
				}
				continue;
			}

			if (key == "default_background" || key == "background_color" || key == "background" || key == "canvas_color") {
				bool valid = false;
				Color parsed = _parse_theme_color(value, valid);
				if (valid) {
					bg = parsed;
					bg_set = true;
				}
				else {
					UtilityFunctions::printerr("[nvim_embed] Theme color '", value, "' for key '", key, "' is invalid in ", path);
				}
				continue;
			}

			if (key == "colorscheme" || key == "command" || key == "theme") {
				colorscheme_name = value;
				continue;
			}
		}

		if (fg_set) {
			theme_default_foreground = fg;
		}
		if (bg_set) {
			theme_default_background = bg;
		}
		if (!colorscheme_name.is_empty()) {
			theme_colorscheme_name = colorscheme_name;
		}

		String file_name = path.get_file();
		loaded_theme_name = file_name.get_basename();
		loaded = true;
		if (reduced != loaded_theme_name && reduced != "default") {
			UtilityFunctions::print("[nvim_embed] Theme '", p_theme_name, "' not found, using '", loaded_theme_name, "'.");
		}
		break;
	}

	if (theme_colorscheme_name.is_empty()) {
		theme_colorscheme_name = original_case_name;
	}

	if (!loaded) {
		if (reduced != "default") {
			UtilityFunctions::printerr("[nvim_embed] Theme '", p_theme_name, "' not found; using default colors.");
		}
		theme_default_foreground = Color(1, 1, 1, 1);
		theme_default_background = Color(0, 0, 0, 1);
		loaded_theme_name = "default";
		theme_colorscheme_name = original_case_name;
	}

	theme_name_setting = loaded_theme_name;
	return loaded;
}

Color NvimPanel::_parse_theme_color(const String &p_value, bool &r_valid) const {
	String cleaned = p_value.strip_edges();
	if (cleaned.is_empty()) {
		r_valid = false;
		return Color();
	}

	if (Color::html_is_valid(cleaned)) {
		r_valid = true;
		return Color::html(cleaned);
	}

	const Color sentinel(-1.0f, -1.0f, -1.0f, -1.0f);
	Color parsed = Color::from_string(cleaned, sentinel);
	if (parsed.a < 0.0f) {
		r_valid = false;
		return Color();
	}

	r_valid = true;
	return parsed;
}

void NvimPanel::_apply_theme_to_running_instance() {
	if (theme_colorscheme_name.is_empty()) {
		return;
	}
	if (!nvim_client || !nvim_client->is_running()) {
		return;
	}

	String command = String("colorscheme ") + theme_colorscheme_name;
	if (!_send_nvim_command(command)) {
		UtilityFunctions::printerr("[nvim_embed] Failed to apply colorscheme: ", theme_colorscheme_name);
	}
}

void NvimPanel::_on_canvas_resized() {
	_sync_neovim_size_to_canvas();
}

void NvimPanel::_ensure_cell_metrics() {
	if (cell_width > 0.0f && cell_height > 0.0f) {
		return;
	}

	Ref<Font> font = _obtain_font();
	if (font.is_null()) {
		return;
	}

	int32_t size = _obtain_font_size();
	Vector2 char_size = font->get_char_size(U'M', size);
	float cw = char_size.x;
	if (cw <= 0.0f) {
		char_size = font->get_char_size(U' ', size);
		cw = char_size.x;
	}
	if (cw <= 0.0f) {
		cw = font->get_height(size) * 0.6f;
	}
	float ch = font->get_height(size);
	float ascent = font->get_ascent(size);

	cell_width = cw > 0.0f ? cw : 1.0f;
	cell_height = ch > 0.0f ? ch : 1.0f;
	cell_ascent = ascent >= 0.0f ? ascent : cell_height * 0.8f;
}

void NvimPanel::reload_settings() {
	const String default_command = "nvim";
	const int32_t default_font_size = 14;
	const bool default_autostart = true;
	const String default_font_path = _get_default_font_path();
	const PackedStringArray default_args;
	const String default_theme = "default";
	const bool default_debug_logging = false;

	ProjectSettings *ps = ProjectSettings::get_singleton();

	String command_value = default_command;
	bool autostart_value = default_autostart;
	String font_path_value = default_font_path;
	int32_t font_size_value = default_font_size;
	PackedStringArray extra_args_value = default_args;
	String theme_value = default_theme;
	bool debug_logging_value = default_debug_logging;

	if (ps) {
		if (ps->has_setting("neovim/embed/command")) {
			Variant v = ps->get_setting("neovim/embed/command");
			if (v.get_type() == Variant::STRING) {
				command_value = (String)v;
			}
		}
		if (ps->has_setting("neovim/embed/autostart")) {
			Variant v = ps->get_setting("neovim/embed/autostart");
			if (v.get_type() == Variant::BOOL) {
				autostart_value = (bool)v;
			}
		}
		if (ps->has_setting("neovim/embed/font_path")) {
			Variant v = ps->get_setting("neovim/embed/font_path");
			if (v.get_type() == Variant::STRING) {
				font_path_value = (String)v;
			}
		}
		if (ps->has_setting("neovim/embed/font_size")) {
			Variant v = ps->get_setting("neovim/embed/font_size");
			if (v.get_type() == Variant::INT) {
				font_size_value = static_cast<int32_t>((int64_t)v);
			}
		}
		if (ps->has_setting("neovim/embed/extra_args")) {
			Variant v = ps->get_setting("neovim/embed/extra_args");
			if (v.get_type() == Variant::PACKED_STRING_ARRAY) {
				extra_args_value = (PackedStringArray)v;
			}
		}
		if (ps->has_setting("neovim/embed/theme")) {
			Variant v = ps->get_setting("neovim/embed/theme");
			if (v.get_type() == Variant::STRING) {
				theme_value = (String)v;
			}
		}
		if (ps->has_setting("neovim/embed/debug_logging")) {
			Variant v = ps->get_setting("neovim/embed/debug_logging");
			if (v.get_type() == Variant::BOOL) {
				debug_logging_value = (bool)v;
			}
		}
	}

	nvim_command = command_value.is_empty() ? default_command : command_value;
	autostart = autostart_value;
	font_path_setting = font_path_value;
	font_size = font_size_value > 0 ? font_size_value : default_font_size;
	extra_args_setting = extra_args_value;
	_load_theme_definition(theme_value);
	debug_logging_enabled = debug_logging_value;
	cached_font.unref();
	const bool running = is_running();
	_apply_theme_defaults(!running);
	if (running) {
		_apply_theme_to_running_instance();
	}
	_update_ui_state();
}

String NvimPanel::_get_default_font_path() const {
	return "res://addons/VimDot/assets/fonts/jetbrains/JetBrainsMonoNerdFontMono-Regular.ttf";
}

bool NvimPanel::_send_nvim_input(const String &p_keys) {
	if (!nvim_client || !nvim_client->is_running()) {
		return false;
	}

	CharString method_utf8 = String("nvim_input").utf8();
	CharString keys_utf8 = p_keys.utf8();

	char *buffer = nullptr;
	size_t buffer_size = 0;
	mpack_writer_t writer;
	mpack_writer_init_growable(&writer, &buffer, &buffer_size);

	uint32_t request_id = next_request_id++;
	mpack_start_array(&writer, 4);
	mpack_write_i32(&writer, 0);
	mpack_write_u32(&writer, request_id);
	mpack_write_cstr(&writer, method_utf8.get_data());
	mpack_start_array(&writer, 1);
	mpack_write_cstr(&writer, keys_utf8.get_data());
	mpack_finish_array(&writer);
	mpack_finish_array(&writer);

	mpack_error_t writer_error = mpack_writer_destroy(&writer);
	if (writer_error != mpack_ok || !buffer || buffer_size == 0) {
		if (buffer) {
			std::free(buffer);
		}
		return false;
	}

	size_t written = nvim_client->write(reinterpret_cast<const uint8_t *>(buffer), buffer_size);
	std::free(buffer);
	return written == buffer_size;
}

bool NvimPanel::_send_nvim_command(const String &p_command) {
	if (!nvim_client || !nvim_client->is_running()) {
		return false;
	}

	CharString method_utf8 = String("nvim_command").utf8();
	CharString command_utf8 = p_command.utf8();

	char *buffer = nullptr;
	size_t buffer_size = 0;
	mpack_writer_t writer;
	mpack_writer_init_growable(&writer, &buffer, &buffer_size);

	uint32_t request_id = next_request_id++;
	mpack_start_array(&writer, 4);
	mpack_write_i32(&writer, 0);
	mpack_write_u32(&writer, request_id);
	mpack_write_cstr(&writer, method_utf8.get_data());
	mpack_start_array(&writer, 1);
	mpack_write_cstr(&writer, command_utf8.get_data());
	mpack_finish_array(&writer);
	mpack_finish_array(&writer);

	mpack_error_t writer_error = mpack_writer_destroy(&writer);
	if (writer_error != mpack_ok || !buffer || buffer_size == 0) {
		if (buffer) {
			std::free(buffer);
		}
		return false;
	}

	size_t written = nvim_client->write(reinterpret_cast<const uint8_t *>(buffer), buffer_size);
	std::free(buffer);
	return written == buffer_size;
}

bool NvimPanel::_send_nvim_input_mouse(const String &p_button, const String &p_action, const String &p_modifiers, int64_t p_grid, int64_t p_row, int64_t p_column) {
	if (!nvim_client || !nvim_client->is_running()) {
		return false;
	}

	CharString method_utf8 = String("nvim_input_mouse").utf8();
	CharString button_utf8 = p_button.utf8();
	CharString action_utf8 = p_action.utf8();
	CharString mod_utf8 = p_modifiers.utf8();

	char *buffer = nullptr;
	size_t buffer_size = 0;
	mpack_writer_t writer;
	mpack_writer_init_growable(&writer, &buffer, &buffer_size);

	uint32_t request_id = next_request_id++;
	mpack_start_array(&writer, 4);
	mpack_write_i32(&writer, 0);
	mpack_write_u32(&writer, request_id);
	mpack_write_cstr(&writer, method_utf8.get_data());
	mpack_start_array(&writer, 6);
	mpack_write_cstr(&writer, button_utf8.get_data());
	mpack_write_cstr(&writer, action_utf8.get_data());
	mpack_write_cstr(&writer, mod_utf8.get_data());
	mpack_write_i64(&writer, p_grid);
	mpack_write_i64(&writer, p_row);
	mpack_write_i64(&writer, p_column);
	mpack_finish_array(&writer);
	mpack_finish_array(&writer);

	mpack_error_t writer_error = mpack_writer_destroy(&writer);
	if (writer_error != mpack_ok || !buffer || buffer_size == 0) {
		if (buffer) {
			std::free(buffer);
		}
		return false;
	}

	size_t written = nvim_client->write(reinterpret_cast<const uint8_t *>(buffer), buffer_size);
	std::free(buffer);
	return written == buffer_size;
}
