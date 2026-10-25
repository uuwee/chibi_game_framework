#include "targets.h"

#include <stddef.h>

const BuildTarget target_self = {
    .type = BUILD_TYPE_EXE,
    .name = "build_runner",
    .output = ".build/bin/build-next.exe",
    .sources = (const char *const[]){"buildsys/build.c", "buildsys/targets.c",
                                     "buildsys/main.c", NULL},
};

const BuildTarget target_runtime = {
    .type = BUILD_TYPE_EXE,
    .name = "runtime",
    .output = ".build/bin/runtime.exe",
    .sources =
        (const char *const[]){"runtime/main.c", "runtime/api_registry.c",
                              "runtime/plugin.c", "buildsys/build.c",
                              "buildsys/targets.c", "engine/engine.c", NULL},
};

const BuildTarget target_sandbox = {
    .type = BUILD_TYPE_DLL,
    .name = "sandbox",
    .output = ".build/bin/game.dll",
    .sources = (const char *const[]){"examples/sandbox/game.c", NULL},
    .include_dirs = (const char *const[]){"engine", NULL},
    .defines = (const char *const[]){"CGF_PLUGIN_DLL", NULL},
};

const BuildTarget target_renderer_shaders = {
    .type = BUILD_TYPE_SHADERS,
    .name = "renderer_shaders",
    .output = ".build/shaders",
    .sources =
        (const char *const[]){
            "engine/shaders/cull_reset.comp", "engine/shaders/cull_count.comp",
            "engine/shaders/cull_prefix.comp", "engine/shaders/cull_write.comp",
            "engine/shaders/light_cull.comp", "engine/shaders/gi_pyramid.comp",
            "engine/shaders/gi_cascade.comp", "engine/shaders/gi_probe.comp",
            "engine/shaders/gi_probe_filter.comp", "engine/shaders/gi_apply.comp",
            "engine/shaders/geometry.vert",
            "engine/shaders/prepass.frag", "engine/shaders/forward.frag",
            "engine/shaders/fullscreen.vert", "engine/shaders/tonemap.frag",
            NULL},
};

const BuildTarget target_renderer = {
    .type = BUILD_TYPE_DLL,
    .name = "renderer",
    .output = ".build/bin/renderer.dll",
    .sources = (const char *const[]){"engine/renderer.c",
                                     "engine/renderer_vulkan.c",
                                     "engine/renderer_frame.c",
                                     "$(VULKAN_SDK)/Include/Volk/volk.c", NULL},
    .include_dirs =
        (const char *const[]){"engine", ".build/shaders",
                              "$(VULKAN_SDK)/Include",
                              "$(VULKAN_SDK)/Include/Volk", NULL},
    .defines = (const char *const[]){"CGF_PLUGIN_DLL",
                                     "VK_USE_PLATFORM_WIN32_KHR", NULL},
    .dependencies = (const BuildTarget *const[]){&target_renderer_shaders, NULL},
};
