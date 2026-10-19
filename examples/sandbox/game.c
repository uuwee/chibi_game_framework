#include <stdio.h>
#include "engine.h"

void greet(){
  printf("hey! this is sandbox example\n");
}

typedef struct State{
  float player_x;
  float player_y;
} State;

