#define _GNU_SOURCE
#include "kpm_broker/kpm_broker.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void stop_handler(int sig) { (void)sig; stopping=1; }
static bool number(const char *s, unsigned *n) {
  if (!s || !*s) return false;
  char *end; errno=0; unsigned long parsed=strtoul(s,&end,10);
  if (errno || *end || parsed<1 || parsed>3600000) return false;
  *n=parsed; return true;
}
int main(int argc, char **argv) {
  setvbuf(stdout,NULL,_IOLBF,0);
  const char *source=NULL,*profile="base5",*config=NULL,*dir=NULL,*evidence=NULL;
  const char *plugin="/usr/local/lib/flexric/libkpm_sm.so";
  unsigned period=1000; bool repeat=false, check=false;
  char **sdk_args=calloc(argc+1,sizeof(*sdk_args)); if (!sdk_args) return 2;
  int sdk_argc=1; sdk_args[0]=argv[0];
  for (int i=1;i<argc;++i) {
    const char *arg=argv[i];
    if (!strcmp(arg,"--fixture-repeat")) { repeat=true; continue; }
    if (!strcmp(arg,"--check-config")) { check=true; continue; }
    if (!strcmp(arg,"-c") || !strcmp(arg,"-p")) {
      if (++i>=argc) goto usage;
      sdk_args[sdk_argc++]=argv[i-1]; sdk_args[sdk_argc++]=argv[i]; continue;
    }
    if (++i>=argc) goto usage;
    if (!strcmp(arg,"--source")) source=argv[i];
    else if (!strcmp(arg,"--metric-profile")) profile=argv[i];
    else if (!strcmp(arg,"--broker-config")) config=argv[i];
    else if (!strcmp(arg,"--fixture-dir")) dir=argv[i];
    else if (!strcmp(arg,"--fixture-plugin")) plugin=argv[i];
    else if (!strcmp(arg,"--callback-evidence")) evidence=argv[i];
    else if (!strcmp(arg,"--fixture-period-ms")) { if (!number(argv[i],&period)) goto usage; }
    else goto usage;
  }
  if (!source || (strcmp(source,"e2") && strcmp(source,"fixture")) || strcmp(profile,"base5") ||
      (!strcmp(source,"fixture") && (!dir || sdk_argc!=1)) ||
      (!strcmp(source,"e2") && (dir || repeat || sdk_argc==1))) goto usage;
  if (check) {
    if (strcmp(source,"e2")) goto usage;
    bool valid=bk_source_check(sdk_argc,sdk_args); free(sdk_args); return valid ? 0 : 1;
  }
  bk_config cfg; bk_defaults(&cfg);
  if (config && !bk_config_load(&cfg,config)) { fprintf(stderr,"Invalid broker configuration\n"); free(sdk_args); return 2; }
  if (!strcmp(source,"e2") && (cfg.write_chunk || cfg.test_stall_app[0])) {
    fprintf(stderr,"Fixture I/O adapters cannot be used with E2\n"); free(sdk_args); return 2;
  }
  bk_context *ctx=calloc(1,sizeof(*ctx));
  if (!ctx || !bk_init(ctx,&cfg,source,evidence)) { perror("broker context"); free(ctx); free(sdk_args); return 2; }
  struct sigaction action={.sa_handler=stop_handler}; sigemptyset(&action.sa_mask);
  sigaction(SIGINT,&action,NULL); sigaction(SIGTERM,&action,NULL); signal(SIGPIPE,SIG_IGN);
  pthread_t worker;
  if (pthread_create(&worker,NULL,bk_server,ctx)) { bk_destroy(ctx); free(ctx); free(sdk_args); return 2; }
  while (!atomic_load(&ctx->ready) && !atomic_load(&ctx->failed) && !stopping) usleep(1000);
  bool ok=!atomic_load(&ctx->failed);
  if (ok && !stopping) {
    ok=!strcmp(source,"fixture") ? bk_fixture_load(ctx,dir,plugin,period,repeat,&stopping) :
      bk_source_run(ctx,sdk_argc,sdk_args,&stopping);
    while (ok && !stopping && !atomic_load(&ctx->failed)) usleep(10000);
  }
  bk_disable(ctx); atomic_store(&ctx->stop,true); bk_wake(ctx); pthread_join(worker,NULL);
  bk_destroy(ctx); ok=ok && !atomic_load(&ctx->failed) && !ctx->serialization_errors && !ctx->evidence_limited;
  free(ctx); free(sdk_args); return ok ? 0 : 1;
usage:
  fprintf(stderr,"Usage: %s --source fixture|e2 --metric-profile base5 [--broker-config FILE] "
    "[--callback-evidence NEW_FILE] (--fixture-dir DIR [--fixture-repeat] [--fixture-period-ms N] "
    "[--fixture-plugin SO] | -c BASE5_CONFIG [-p SM_DIR] [--check-config])\n",argv[0]);
  free(sdk_args); return 2;
}
