import os
import platform

env = Environment(ENV=os.environ)

sys = platform.system().lower()
plat = ARGUMENTS.get("platform", "macos" if sys.startswith("darwin") else "linux")
target_kind = ARGUMENTS.get("target", "template_release")  # template_debug / template_release / editor
archs = ARGUMENTS.get("arch", "arm64")

cxxflags = ["-std=c++17", "-fPIC"]
ccflags = ["-fPIC"]
defines = []
linkflags = []

if "debug" in target_kind:
    defines += ["DEBUG_ENABLED"]
    cxxflags += ["-g"]
else:
    cxxflags += ["-O3"]

GODOTCPP = "external/godot-cpp"

env.Append(CPPPATH=[
    "include",
    "thirdparty/mpack",
    os.path.join(GODOTCPP, "gen", "include"),
    os.path.join(GODOTCPP, "include"),
    os.path.join(GODOTCPP, "gdextension"),
])

if plat == "macos":
    env["SHLIBSUFFIX"] = ".dylib"
    cxxflags += ["-mmacosx-version-min=11.0"]
    linkflags += ["-mmacosx-version-min=11.0", "-Wl,-rpath,@loader_path"]
    for a in archs.split(","):
        cxxflags += ["-arch", a]
        linkflags += ["-arch", a]
    if "debug" in target_kind:
        cand = os.path.join(GODOTCPP, "bin", "libgodot-cpp.macos.template_debug.universal.a")
        if not os.path.exists(cand):
            cand = os.path.join(GODOTCPP, "bin", "libgodot-cpp.macos.template_debug.arm64.a")
    else:
        cand = os.path.join(GODOTCPP, "bin", "libgodot-cpp.macos.template_release.arm64.a")
    linkflags += [cand]

elif plat == "linux":
    env["SHLIBSUFFIX"] = ".so"
    linkflags += ["-Wl,-rpath,$ORIGIN"]
    cand = os.path.join(GODOTCPP, "bin", "linux", "x86_64", "libgodot-cpp.linux." + ("template_debug" if "debug" in target_kind else "template_release") + ".x86_64.a")
    if os.path.exists(cand):
        linkflags += [cand]

else:
    Exit("Unsupported platform: " + plat)

env.Append(CXXFLAGS=cxxflags, CCFLAGS=ccflags, CPPDEFINES=defines, LINKFLAGS=linkflags)

srcs = [
    "src/register_types.cpp",
    "src/nvim_client.cpp",
    "src/nvim_editor_plugin.cpp",
    "src/nvim_panel.cpp",
    "thirdparty/mpack/mpack-common.c",
    "thirdparty/mpack/mpack-expect.c",
    "thirdparty/mpack/mpack-node.c",
    "thirdparty/mpack/mpack-platform.c",
    "thirdparty/mpack/mpack-reader.c",
    "thirdparty/mpack/mpack-writer.c",
]

out_dir = "bin"
os.makedirs(out_dir, exist_ok=True)

name_base = "libnvim_embed"
if plat == "macos":
    arch_tag = "universal" if ("," in archs) else archs
    fname = f"{name_base}.macos.{target_kind}.{arch_tag}"
else:
    fname = f"{name_base}.linux.{target_kind}.x86_64"
# Ensure platform-specific shared library suffix is included in the target filename.
target_path = f"{out_dir}/{fname}{env['SHLIBSUFFIX']}"

lib = env.SharedLibrary(target=target_path, source=srcs)
Default(lib)
