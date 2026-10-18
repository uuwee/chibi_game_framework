#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <direct.h>
#include <process.h>

typedef struct BuildTarget {
  const char *name;
  const char *source;
  const char *object;
  const char *output;
} BuildTarget;

static int run_process(const char *const args[]) {
  printf("[CMD]");

  for (int i = 0; args[i] != NULL; i++) {
    printf(" %s", args[i]);
  }

  printf("\n");
  fflush(stdout);

  intptr_t result = _spawnvp(_P_WAIT, args[0], args);

  if(result == -1){
    perror("[ERROR] Process");
    return 1;
  }

  if (result != 0){
    fprintf(stderr, "[ERROR] Exit code: %d\n", (int) result);
    return 1;
  }
  return 0;
}

static int make_directory(const char *path){
  if (_mkdir(path) == 0){
    return 0;
  }

  if (errno == EEXIST){
    return 0;
  }

  fprintf(stderr, "[ERROR] Cannot create: %s\n", path);
  return 1;
}

static int prepare_directories(void){
  if (make_directory(".build")) return 1;
  if (make_directory(".build/obj")) return 1;
  if (make_directory(".build/bin")) return 1;

  return 0;
}

static int compile_target(const BuildTarget *target){
  char object_flag[512];
  int length = snprintf(object_flag, sizeof(object_flag), "/Fo%s", target->object);

  if(length < 0 || (size_t)length >= sizeof(object_flag)){
    return 1;
  }

  const char* const args[] = {
    "cl.exe", "/nologo", "/W4", "/std:clatest", "/Od", "/MD", "/c", target->source, object_flag, NULL
  };

  return run_process(args);
}

static int link_target(const BuildTarget *target){
  char output_flag[512];

  int length = snprintf(output_flag, sizeof(output_flag), "/OUT:%s", target->output);

  if (length < 0 || (size_t)length >= sizeof(output_flag)){
    return 1;
  }

  const char* const args[] = {
    "link.exe", "/NOLOGO", "/INCREMENTAL:NO", target->object, output_flag, NULL
  };

  return run_process(args);
}

static int build_target(const BuildTarget *target){
  printf("[BUILD] %s\n", target->name);

  if(prepare_directories()){return 1;}

  if(compile_target(target)){return 1;}

  if(link_target(target)){return 1;}

  printf("[SUCCESS] %s\n", target->output);

  return 0;
}

static const BuildTarget target_self = {
  "build_runner", "buildsys/build.c", ".build/obj/runtime.obj", ".build/bin/runtime.exe"
};

static const BuildTarget target_runtime = {
  "runtime", "runtime/main.c", ".build/obj/runtime.obj", ".build/bin/runtime.exe"
};

int main(int argc, char **argv){
  const char *command = "all";

  if(argc >= 2){
    command = argv[1];
  }

  if (argc > 2) {
    printf("Too many arguments.\n");
    return 1;
  }

  if (strcmp(command, "self") == 0){
    return build_target(&target_self);
  }

  if (strcmp(command, "runtime") == 0){return build_target(&target_runtime);}

  if (strcmp(command, "all") == 0){
    if (build_target(&target_self)){
      return 1;
    }

    return build_target(&target_runtime);
  }

  printf("Usage: build [self | runtime | all]\n");
  return 1;
}

