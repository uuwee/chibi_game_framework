#include "build.h"

#include <stdio.h>
#include <string.h>

static const BuildTarget target_self = {
    .type = BUILD_TYPE_EXE,
    .name = "build_runner",
    .source = (const char *[]){"buildsys/build.c", "buildsys/main.c"},
    .source_count = 2,
    .output = ".build/bin/build-next.exe",
};

static const BuildTarget target_runtime = {
    .type = BUILD_TYPE_EXE,
    .name = "runtime",
    .source = (const char *[]){"runtime/main.c", "runtime/api_registry.c",
                               "runtime/plugin.c", "buildsys/build.c", "engine/engine.c"},
    .source_count = 5,
    .output = ".build/bin/runtime.exe",
};

int main(int argc, char **argv) {
  const char *command = "all";

  if (argc >= 2) {
    command = argv[1];
  }

  if (argc > 2) {
    printf("Too many arguments.\n");
    return 1;
  }

  if (strcmp(command, "self") == 0) {
    return build_target(&target_self);
  }

  if (strcmp(command, "runtime") == 0) {
    return build_target(&target_runtime);
  }

  if (strcmp(command, "all") == 0) {
    if (build_target(&target_self)) {
      return 1;
    }

    return build_target(&target_runtime);
  }

  printf("Usage: build [self | runtime | all]\n");
  return 1;
}
