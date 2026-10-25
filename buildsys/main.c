#include "build.h"
#include "targets.h"

#include <stdio.h>
#include <string.h>

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

  if (strcmp(command, "sandbox") == 0) {
    return build_target(&target_sandbox);
  }

  if (strcmp(command, "renderer") == 0) {
    return build_target(&target_renderer);
  }

  if (strcmp(command, "all") == 0) {
    if (build_target(&target_self)) {
      return 1;
    }

    if (build_target(&target_runtime)) {
      return 1;
    }

    return build_target(&target_renderer);
  }

  printf("Usage: build [self | runtime | sandbox | renderer | all]\n");
  return 1;
}
