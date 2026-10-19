#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <direct.h>
#include <process.h>

#include "build.h"

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

static int compile_unit(const CompileUnit *unit, const char *option) {
  char object_flag[512];
  int length =
      snprintf(object_flag, sizeof(object_flag), "/Fo%s", unit->object);

  if (length < 0 || (size_t)length >= sizeof(object_flag)) {
    return 1;
  }

  const char *const args[] = {"cl.exe",       "/nologo",   "/W4",
                              "/std:clatest", "/Od",       "/MD",
                              "/c",           "/Z7",       option ? option : "",
                              unit->source,   object_flag, NULL};

  return run_process(args);
}

static int make_object_path(const char *source, char *buffer,
                            size_t buffer_size) {
  char name[512];
  int copied = snprintf(name, sizeof(name), "%s", source);
  if (copied < 0 || (size_t)copied >= sizeof(name)) return 1;
  for (char *p = name; *p; ++p) {
    if (*p == '/' || *p == '\\' || *p == ':') *p = '_';
  }
  int length =
      snprintf(buffer, buffer_size, ".build/obj/%s.obj", name);
  if (length < 0 || (size_t)length >= buffer_size) {
    return 1;
  }

  return 0;
}

static int link_target(const BuildTarget *target, const CompileUnit *units,
                       size_t unit_count) {
  char object_path_list[1024 * 8] = {0};
  char *head = object_path_list;
  size_t remaining = sizeof(object_path_list);
  for (size_t i = 0; i < unit_count; i++) {
    int length =
        snprintf(head, remaining, "\"%s\" ", units[i].object);
    if (length < 0 || (size_t)length >= remaining) return 1;
    head += length;
    remaining -= (size_t)length;
  }

  char output_flag[512];
  int length = -1;
  if (target->type == BUILD_TYPE_EXE) {
    // TODO: check if target->output ends with ".exe"
    length =
        snprintf(output_flag, sizeof(output_flag), "/OUT:\"%s\"", target->output);
  } else if (target->type == BUILD_TYPE_DLL) {
    // TODO: check if target->output ends with ".dll"
    length = snprintf(output_flag, sizeof(output_flag), "/DLL /OUT:\"%s\"",
                      target->output);
  }
  if (length < 0 || (size_t)length >= sizeof(output_flag)) {
    return 1;
  }

  const char *const args[] = {"link.exe",       "/NOLOGO",   "/INCREMENTAL:NO",
                              object_path_list, output_flag, NULL};

  return run_process(args);
}

int build_target(const BuildTarget *target) {
  printf("[BUILD] %s\n", target->name);

  if (prepare_directories()) {
    return 1;
  }

  uint32_t compile_units_count = target->source_count;
  CompileUnit *compile_units =
      (CompileUnit *)malloc(sizeof(CompileUnit) * compile_units_count);
  if (!compile_units) {
    printf("[ERROR] failed to allocate compile units\n");
    return 1;
  }

  for (uint32_t i = 0; i < compile_units_count; i++) {
    compile_units[i].source = target->source[i];
    const int result =
        make_object_path(target->source[i], compile_units[i].object,
                         sizeof(compile_units[i].object));
    if (result != 0) {
      printf("[ERROR] failed to create object path: %d\n", result);
      free(compile_units);
      return 1;
    }
    printf("obj path: %s\n", compile_units[i].object);

    if (compile_unit(&compile_units[i], target->option)) {
      free(compile_units);
      return 1;
    }
  }

  int link_result = link_target(target, compile_units, compile_units_count);
  free(compile_units);
  if (link_result) {
    return 1;
  }

  printf("[SUCCESS] %s\n", target->output);

  return 0;
}
