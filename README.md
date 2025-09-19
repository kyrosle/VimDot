<img src="./assets/Icon 1024.png" width="150px" alt="Project Logo" />

# Neovim Embed for Godot
---

Bring the full Neovim experience directly into the Godot editor. This addon exposes a main-screen tab that hosts a `--embed` Neovim session, letting you keep your modal editing habits, plugins, and colorschemes while developing scenes and scripts.

## Highlights

- **Dockable Neovim panel** – Works like Godot’s native script workspace, including focus handling and editor sizing.
- **Colorscheme aware** – Project settings map to `.theme` files that translate into Neovim `+colorscheme` CLI flags, so the embedded instance matches your terminal look immediately.
- **Tools menu actions** – Quick access to “Open Current File in Neovim”, `:w`, autostart toggles, and restart commands.
- **Crash-safe overlay** – If Neovim exits, the panel shows a restart button and reapplies theme settings on relaunch.
- **Optional Godot script editor replacement** – Experimental flag hides the built-in editor so script double-clicks route straight into Neovim.
- **Opt-in debug logging** – Enable `neovim/embed/debug_logging` to trace incoming RPC batches and redraw events.

## Installation

1. Copy `addons/nvim_embed/` into your Godot project.
2. Enable the plugin in **Project > Project Settings > Plugins**.
3. Ensure `nvim` is available on your PATH (or adjust the command in project settings).

## Usage

- Switch to the **Neovim** tab to spawn the embedded session (autostart is configurable).
- Adjust behaviour via **Project Settings > neovim/embed/**:
  - `autostart` – start Neovim automatically with the editor.
  - `command` / `extra_args` – customize the binary and launch flags.
  - `font_path` / `font_size` – tweak the panel’s font rendering.
  - `theme` – pick a `.theme` file (e.g. `tokyo_night`, `gruvbox`).
  - `hide_script_editor_experimental` – hide Godot’s script tab and hijack script double-clicks.
  - `debug_logging` – emit `[nvim_embed] …` tracing for debugging.

## Theming

Theme descriptors live under `addons/nvim_embed/themes/` and look like:

```
# Tokyonight (Mocha)
default_foreground=#c0caf5
default_background=#1a1b26
colorscheme=tokyonight
```

When selected, the addon launches Neovim as:

```sh
nvim --embed +"colorscheme tokyonight"
```

You can create new `.theme` files to match custom colorschemes or distributions like LazyVim. The `default_foreground`/`default_background` keys control the panel colors before Neovim attaches.

## Troubleshooting

- **Neovim fails to start** – confirm the `command` points to a Neovim build with `--embed` support (0.9+) and that it’s executable in the project’s environment.
- **Theme doesn’t stick** – ensure the colourscheme name in the `.theme` file matches what `:colorscheme` expects. User configs overriding `colorscheme` in `init.lua` may reapply their own palette.
- **RPC trace noise** – disable `debug_logging` when you don’t need the message flow.

## License

- Neovim Embed plugin code: MIT License (see `addons/nvim_embed/LICENSE.txt`).
- MPack library (MessagePack helper) in `addons/nvim_embed/thirdparty/mpack/`: MIT License by its original authors.
