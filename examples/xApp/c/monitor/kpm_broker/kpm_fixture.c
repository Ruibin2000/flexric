#include "kpm_broker.h"
#include "sm/sm_ric.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char *read_file(const char *path, size_t *len) {
  FILE *f=fopen(path,"rb"); if (!f) return NULL;
  unsigned char *bytes=malloc(2*1024*1024+1); if (!bytes) { fclose(f); return NULL; }
  *len=fread(bytes,1,2*1024*1024+1,f); bool ok=!ferror(f) && *len && *len<=2*1024*1024; fclose(f);
  if (!ok) { free(bytes); return NULL; } return bytes;
}
static bool path_for(char *out, size_t size, const char *dir, const char *name) {
  return bk_identifier(name) && snprintf(out,size,"%s/%s",dir,name)>0 && strlen(dir)+strlen(name)+2<=size;
}
bool bk_fixture_load(bk_context *ctx, const char *dir, const char *plugin, unsigned period_ms,
                     bool repeat, volatile sig_atomic_t *stopping) {
  char path[4096]; if (snprintf(path,sizeof(path),"%s/manifest.json",dir)>=(int)sizeof(path)) return false;
  size_t len; unsigned char *bytes=read_file(path,&len); if (!bytes) return false;
  json_object *manifest=bk_parse((const char*)bytes,len); free(bytes);
  const char *profile=bk_string(manifest,"profile"); json_object *cases=NULL;
  bool ok=profile && !strcmp(profile,"base5") && json_object_object_get_ex(manifest,"cases",&cases) &&
    json_object_is_type(cases,json_type_array) && json_object_array_length(cases)>0 && json_object_array_length(cases)<=32;
  if (!ok) { json_object_put(manifest); return false; }
  /* KPM ASN symbols stay in their plugin; the executable uses the SDK's E2AP runtime. */
  void *library=dlopen(plugin,RTLD_NOW|RTLD_LOCAL);
  if (!library) { fprintf(stderr,"fixture plugin: %s\n",dlerror()); json_object_put(manifest); return false; }
  sm_ric_t *(*make)(void)=dlsym(library,"make_kpm_sm_ric");
  if (!make) { dlclose(library); json_object_put(manifest); return false; }
  sm_ric_t *model=make();
  do {
    for (size_t i=0;i<json_object_array_length(cases) && !*stopping && !atomic_load(&ctx->failed);++i) {
      uint64_t deadline=bk_mono_ms()+period_ms;
      while (bk_mono_ms()<deadline && !*stopping && !atomic_load(&ctx->failed)) usleep(1000);
      if (*stopping || atomic_load(&ctx->failed)) break;
      json_object *item=json_object_array_get_idx(cases,i), *link=NULL;
      bk_key key; sm_ind_data_t raw={0};
      if (!json_object_object_get_ex(item,"link",&link) || !bk_key_parse(link,&key) ||
          !path_for(path,sizeof(path),dir,bk_string(item,"header")) || !(raw.ind_hdr=read_file(path,&raw.len_hdr)) ||
          !path_for(path,sizeof(path),dir,bk_string(item,"message")) || !(raw.ind_msg=read_file(path,&raw.len_msg))) {
        free(raw.ind_hdr); free(raw.ind_msg); ok=false; break;
      }
      sm_ag_if_rd_t rd={.type=INDICATION_MSG_AGENT_IF_ANS_V0};
      rd.ind=model->proc.on_indication(model,&raw);
      global_e2_node_id_t node={.type=e2ap_ngran_gNB_DU,.cu_du_id=&key.du};
      node.plmn.mcc=(uint16_t)strtoul(key.mcc,NULL,10); node.plmn.mnc=(uint16_t)strtoul(key.mnc,NULL,10);
      node.plmn.mnc_digit_len=strlen(key.mnc); node.nb_id.nb_id=key.gnb; node.nb_id.unused=32-key.bits;
      bk_callback(ctx,&rd,&node);
      /* Destroy decoded buffers immediately; the queued rows own every field. */
      model->alloc.free_ind_data(&rd.ind); memset(&rd,0xa5,sizeof(rd));
      free(raw.ind_hdr); free(raw.ind_msg);
    }
  } while (ok && repeat && !*stopping && !atomic_load(&ctx->failed));
  model->free_sm(model); dlclose(library); json_object_put(manifest); return ok;
}
