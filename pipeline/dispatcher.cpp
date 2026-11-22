#include <unistd.h>
#include <cstdio>
int main(int argc,char** argv,char** envp){
#if defined(__x86_64__)
  __builtin_cpu_init();
  const char* path="/app/engine";
  if(__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512vl")) path="/app/engine-v4";
  else if(__builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2")) path="/app/engine-v3";
#else
  const char* path="/app/engine";
#endif
  execve(path,argv,envp);
  perror("execve");
  return 127;
}
