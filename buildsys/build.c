#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <direct.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "build.h"

#define BUILD_PATH_MAX 512
#define BUILD_ARG_MAX 4096
/* CreateProcess limit, including the terminating NUL. */
#define BUILD_COMMAND_LINE_MAX 32767

typedef struct CompileUnit {
  const char *source;
  char object[BUILD_PATH_MAX];
} CompileUnit;

typedef struct CommandLine {
  char *data;
  size_t length;
  size_t capacity;
} CommandLine;

static int command_append(CommandLine *command, const char *text,
                          size_t length) {
  if (command->length + length >= BUILD_COMMAND_LINE_MAX) {
    fprintf(stderr, "[ERROR] Command line is too long\n");
    return 1;
  }
  if (command->length + length + 1 > command->capacity) {
    size_t capacity = command->capacity ? command->capacity : 256;
    while (command->length + length + 1 > capacity)
      capacity *= 2;
    char *data = realloc(command->data, capacity);
    if (!data) {
      fprintf(stderr, "[ERROR] Cannot allocate command line\n");
      return 1;
    }
    command->data = data;
    command->capacity = capacity;
  }
  memcpy(command->data + command->length, text, length);
  command->length += length;
  command->data[command->length] = '\0';
  return 0;
}

static int command_append_repeat(CommandLine *command, char c, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (command_append(command, &c, 1))
      return 1;
  }
  return 0;
}

/* Quotes an argument so that the MSVC CRT parses it back unchanged. */
static int command_append_quoted(CommandLine *command, const char *arg) {
  bool quote = arg[0] == '\0' || strpbrk(arg, " \t\n\v\"") != NULL;
  if (!quote)
    return command_append(command, arg, strlen(arg));

  if (command_append(command, "\"", 1))
    return 1;
  for (const char *p = arg;; p++) {
    size_t backslashes = 0;
    while (*p == '\\') {
      p++;
      backslashes++;
    }
    if (*p == '\0') {
      /* Escape trailing backslashes so they do not escape the closing quote. */
      if (command_append_repeat(command, '\\', backslashes * 2))
        return 1;
      break;
    }
    if (*p == '"') {
      if (command_append_repeat(command, '\\', backslashes * 2 + 1) ||
          command_append(command, "\"", 1))
        return 1;
    } else {
      if (command_append_repeat(command, '\\', backslashes) ||
          command_append(command, p, 1))
        return 1;
    }
  }
  return command_append(command, "\"", 1);
}

/* Replaces each $(NAME) in input with the environment variable NAME. */
static int expand_variables(const char *input, char *output,
                            size_t output_size) {
  size_t length = 0;
  const char *p = input;
  while (*p) {
    if (p[0] == '$' && p[1] == '(') {
      const char *name_begin = p + 2;
      const char *name_end = strchr(name_begin, ')');
      char name[128];
      size_t name_length = name_end ? (size_t)(name_end - name_begin) : 0;
      if (name_length == 0 || name_length >= sizeof(name)) {
        fprintf(stderr, "[ERROR] Invalid variable reference: %s\n", input);
        return 1;
      }
      memcpy(name, name_begin, name_length);
      name[name_length] = '\0';

      DWORD value_length = GetEnvironmentVariableA(
          name, output + length, (DWORD)(output_size - length));
      if (value_length == 0) {
        fprintf(stderr, "[ERROR] Environment variable is not set: %s\n", name);
        return 1;
      }
      if (value_length >= output_size - length) {
        fprintf(stderr, "[ERROR] Expanded string is too long: %s\n", input);
        return 1;
      }
      length += value_length;
      p = name_end + 1;
    } else {
      if (length + 1 >= output_size) {
        fprintf(stderr, "[ERROR] Expanded string is too long: %s\n", input);
        return 1;
      }
      output[length++] = *p++;
    }
  }
  output[length] = '\0';
  return 0;
}

/* Appends prefix + value as one argument, expanding variables in value. */
static int command_add(CommandLine *command, const char *prefix,
                       const char *value) {
  char arg[BUILD_ARG_MAX];
  size_t prefix_length = strlen(prefix);
  if (prefix_length >= sizeof(arg)) {
    fprintf(stderr, "[ERROR] Argument is too long: %s\n", prefix);
    return 1;
  }
  memcpy(arg, prefix, prefix_length);
  if (expand_variables(value, arg + prefix_length,
                       sizeof(arg) - prefix_length))
    return 1;

  if (command->length > 0 && command_append(command, " ", 1))
    return 1;
  return command_append_quoted(command, arg);
}

static int command_add_list(CommandLine *command, const char *prefix,
                            const char *const *list) {
  for (size_t i = 0; list && list[i]; i++) {
    if (command_add(command, prefix, list[i]))
      return 1;
  }
  return 0;
}

static int run_command(const CommandLine *command) {
  printf("[CMD] %s\n", command->data);
  fflush(stdout);

  STARTUPINFOA startup = {.cb = sizeof(startup)};
  PROCESS_INFORMATION process = {0};
  /* Inherit handles so redirected stdout/stderr reach the tools as well. */
  if (!CreateProcessA(NULL, command->data, NULL, NULL, TRUE, 0, NULL, NULL,
                      &startup, &process)) {
    fprintf(stderr, "[ERROR] Cannot start process (Windows error %lu)\n",
            GetLastError());
    return 1;
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
    fprintf(stderr, "[ERROR] Cannot get exit code (Windows error %lu)\n",
            GetLastError());
    exit_code = 1;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  if (exit_code != 0) {
    fprintf(stderr, "[ERROR] Exit code: %lu\n", exit_code);
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

static int prepare_directories(const char *object_directory) {
  if (make_directory(".build"))
    return 1;
  if (make_directory(".build/obj"))
    return 1;
  if (make_directory(object_directory))
    return 1;
  if (make_directory(".build/bin"))
    return 1;

  return 0;
}

static int compile_unit(const BuildTarget *target, const CompileUnit *unit) {
  static const char *const base_args[] = {
      "cl.exe", "/nologo", "/W4", "/std:clatest", "/Od",
      "/MD",    "/utf-8",  "/c",  "/Z7",          NULL};

  CommandLine command = {0};
  int result = command_add_list(&command, "", base_args) ||
               command_add_list(&command, "/I", target->include_dirs) ||
               command_add_list(&command, "/D", target->defines) ||
               command_add(&command, "", unit->source) ||
               command_add(&command, "/Fo", unit->object) ||
               run_command(&command);
  free(command.data);
  return result;
}

/* Builds the object path from the unexpanded source, so object names do not
   depend on environment variables. */
static int make_object_path(const char *object_directory, const char *source,
                            char *buffer, size_t buffer_size) {
  char name[BUILD_PATH_MAX];
  int copied = snprintf(name, sizeof(name), "%s", source);
  if (copied < 0 || (size_t)copied >= sizeof(name))
    return 1;
  for (char *p = name; *p; ++p) {
    bool keep = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '.' || *p == '-';
    if (!keep)
      *p = '_';
  }
  int length =
      snprintf(buffer, buffer_size, "%s/%s.obj", object_directory, name);
  if (length < 0 || (size_t)length >= buffer_size) {
    return 1;
  }

  return 0;
}

static int link_target(const BuildTarget *target, const CompileUnit *units,
                       size_t unit_count) {
  if (target->type != BUILD_TYPE_EXE && target->type != BUILD_TYPE_DLL) {
    fprintf(stderr, "[ERROR] Unknown build type: %d\n", (int)target->type);
    return 1;
  }

  CommandLine command = {0};
  int result = command_add(&command, "", "link.exe") ||
               command_add(&command, "", "/NOLOGO") ||
               command_add(&command, "", "/INCREMENTAL:NO") ||
               (target->type == BUILD_TYPE_DLL &&
                command_add(&command, "", "/DLL")) ||
               command_add(&command, "/OUT:", target->output);
  for (size_t i = 0; !result && i < unit_count; i++) {
    result = command_add(&command, "", units[i].object);
  }
  result = result || command_add_list(&command, "", target->link_options) ||
           run_command(&command);
  free(command.data);
  return result;
}

static const char *file_name(const char *path) {
  const char *name = path;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\')
      name = p + 1;
  }
  return name;
}

static int compile_shaders(const BuildTarget *target) {
  if (make_directory(target->output))
    return 1;

  for (size_t i = 0; target->sources[i]; i++) {
    char output[BUILD_PATH_MAX];
    int length = snprintf(output, sizeof(output), "%s/%s.inc", target->output,
                          file_name(target->sources[i]));
    if (length < 0 || (size_t)length >= sizeof(output)) {
      fprintf(stderr, "[ERROR] Shader output path is too long: %s\n",
              target->sources[i]);
      return 1;
    }

    CommandLine command = {0};
    int result =
        command_add(&command, "", "$(VULKAN_SDK)/Bin/glslc.exe") ||
        command_add(&command, "", "--target-env=vulkan1.3") ||
        command_add(&command, "", "-O") ||
        command_add(&command, "", "-mfmt=c") ||
        command_add_list(&command, "-I", target->include_dirs) ||
        command_add_list(&command, "-D", target->defines) ||
        command_add(&command, "", "-o") || command_add(&command, "", output) ||
        command_add(&command, "", target->sources[i]) || run_command(&command);
    free(command.data);
    if (result)
      return 1;
  }
  return 0;
}

int build_target(const BuildTarget *target) {
  if (!target || !target->name || !target->name[0] || !target->output ||
      !target->sources || !target->sources[0]) {
    fprintf(stderr, "[ERROR] Invalid build target\n");
    return 1;
  }

  for (size_t i = 0; target->dependencies && target->dependencies[i]; i++) {
    if (build_target(target->dependencies[i]))
      return 1;
  }

  printf("[BUILD] %s\n", target->name);

  if (target->type == BUILD_TYPE_SHADERS) {
    if (make_directory(".build") || compile_shaders(target))
      return 1;
    printf("[SUCCESS] %s\n", target->output);
    return 0;
  }

  char object_directory[BUILD_PATH_MAX];
  int length = snprintf(object_directory, sizeof(object_directory),
                        ".build/obj/%s", target->name);
  if (length < 0 || (size_t)length >= sizeof(object_directory)) {
    fprintf(stderr, "[ERROR] Target name is too long: %s\n", target->name);
    return 1;
  }

  if (prepare_directories(object_directory)) {
    return 1;
  }

  size_t compile_units_count = 0;
  while (target->sources[compile_units_count])
    compile_units_count++;

  CompileUnit *compile_units =
      (CompileUnit *)malloc(sizeof(CompileUnit) * compile_units_count);
  if (!compile_units) {
    fprintf(stderr, "[ERROR] Cannot allocate compile units\n");
    return 1;
  }

  for (size_t i = 0; i < compile_units_count; i++) {
    compile_units[i].source = target->sources[i];
    if (make_object_path(object_directory, target->sources[i],
                         compile_units[i].object,
                         sizeof(compile_units[i].object))) {
      fprintf(stderr, "[ERROR] Cannot create object path: %s\n",
              target->sources[i]);
      free(compile_units);
      return 1;
    }

    if (compile_unit(target, &compile_units[i])) {
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
