#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <direct.h>
#include <process.h>

typedef struct BuildTarget {
  const char *name;
  const char **source;
  const uint32_t source_count;
  const char *output;
} BuildTarget;

typedef struct CompileUnit {
  const char *source;
  char object[512];
} CompileUnit;

static int run_process(const char *const args[]) {
  printf("[CMD]");

  for (int i = 0; args[i] != NULL; i++) {
    printf(" %s", args[i]);
  }

  printf("\n");
  fflush(stdout);

  intptr_t result = _spawnvp(_P_WAIT, args[0], args);

  if (result == -1) {
    perror("[ERROR] Process");
    return 1;
  }

  if (result != 0) {
    fprintf(stderr, "[ERROR] Exit code: %d\n", (int)result);
    return 1;
  }
  return 0;
}

static int make_directory(const char *path) {
  if (_mkdir(path) == 0) {
    return 0;
  }

  if (errno == EEXIST) {
    return 0;
  }

  fprintf(stderr, "[ERROR] Cannot create: %s\n", path);
  return 1;
}

static int prepare_directories(void) {
  if (make_directory(".build"))
    return 1;
  if (make_directory(".build/obj"))
    return 1;
  if (make_directory(".build/bin"))
    return 1;

  return 0;
}

static int compile_unit(const CompileUnit *unit) {
  char object_flag[512];
  int length =
      snprintf(object_flag, sizeof(object_flag), "/Fo%s", unit->object);

  if (length < 0 || (size_t)length >= sizeof(object_flag)) {
    return 1;
  }

  const char *const args[] = {"cl.exe",     "/nologo",   "/W4", "/std:clatest",
                              "/Od",        "/MD",       "/c",  "/Z7",
                              unit->source, object_flag, NULL};

  return run_process(args);
}

static char *get_filename(const char *filepath) {
  char *baskslash = strrchr(filepath, '/');

  char *filename = (char *)filepath;
  filename = baskslash + 1;
  return filename;
}

static int make_object_path(const char *source, char *buffer,
                            size_t buffer_size) {
  int length =
      snprintf(buffer, buffer_size, ".build/obj/%s.obj", get_filename(source));
  if (length < 0 || (size_t)length >= buffer_size) {
    return 1;
  }

  return 0;
}

static int link_target(const BuildTarget *target, const CompileUnit *units,
                       size_t unit_count) {
  char object_path_list[1024 * 8];
  char *head = object_path_list;
  for (size_t i = 0; i < unit_count; i++) {
    int length =
        snprintf(head, sizeof(object_path_list), "%s ", units[i].object);
    head += length;
  }

  char output_flag[512];
  int length =
      snprintf(output_flag, sizeof(output_flag), "/OUT:%s", target->output);

  if (length < 0 || (size_t)length >= sizeof(output_flag)) {
    return 1;
  }

  const char *const args[] = {"link.exe",      "/NOLOGO",   "/INCREMENTAL:NO",
                              object_path_list, output_flag, NULL};

  return run_process(args);
}

static int build_target(const BuildTarget *target) {
  printf("[BUILD] %s\n", target->name);

  if (prepare_directories()) {
    return 1;
  }

  uint32_t compile_units_count = target->source_count;
  CompileUnit *compile_units =
      (CompileUnit *)malloc(sizeof(CompileUnit) * compile_units_count);
  if (!compile_units) {
    printf("[ERROR] failed to allocate compile units\n");
  }

  for (uint32_t i = 0; i < compile_units_count; i++) {
    compile_units[i].source = target->source[i];
    const int result =
        make_object_path(target->source[i], compile_units[i].object,
                         sizeof(compile_units[i].object));
    if (result != 0) {
      printf("[ERROR] failed to create object path: %d\n", result);
    }
    printf("obj path: %s\n", compile_units[i].object);

    if (compile_unit(&compile_units[i])) {
      return 1;
    }
  }

  if (link_target(target, compile_units, compile_units_count)) {
    return 1;
  }

  printf("[SUCCESS] %s\n", target->output);

  return 0;
}

static const BuildTarget target_self = {
    .name = "build_runner",
    .source = (const char *[]){"buildsys/build.c"},
    .source_count = 1,
    .output = ".build/bin/runtime.exe",
};

static const BuildTarget target_runtime = {
    .name = "runtime",
    .source = (const char *[]){"runtime/main.c", "runtime/sub.c"},
    .source_count = 2,
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
