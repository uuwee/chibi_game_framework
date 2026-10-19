#include <stdio.h>

extern void greet();

int main(){
  printf("hello! I'm runtime\n");
  greet();
  return 0;
}
