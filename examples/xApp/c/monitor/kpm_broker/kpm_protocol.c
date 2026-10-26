#include "kpm_broker.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

const char *bk_string(json_object *obj, const char *name) {
  json_object *value = NULL;
  if (!obj || !json_object_object_get_ex(obj, name, &value) || !json_object_is_type(value, json_type_string)) return NULL;
  const char *s = json_object_get_string(value);
  return strlen(s) == (size_t)json_object_get_string_len(value) ? s : NULL;
}
bool bk_identifier(const char *s) {
  if (!s || !*s || strlen(s) > 64) return false;
  for (; *s; ++s) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
      (*s >= '0' && *s <= '9') || *s == '_' || *s == '.' || *s == '-')) return false;
  return true;
}
bool bk_uint(json_object *obj, const char *name, uint64_t max, uint64_t *value) {
  const char *s = bk_string(obj, name);
  if (!s || !*s || (s[0] == '0' && s[1])) return false;
  uint64_t n = 0;
  for (; *s; ++s) {
    if (*s < '0' || *s > '9' || n > max/10 || (n == max/10 && (unsigned)(*s-'0') > max%10)) return false;
    n = n*10 + *s-'0';
  }
  *value = n; return true;
}
static bool digits(const char *s, size_t low, size_t high) {
  if (!s || strlen(s) < low || strlen(s) > high) return false;
  for (; *s; ++s) if (*s < '0' || *s > '9') return false;
  return true;
}
bool bk_key_parse(json_object *obj, bk_key *key) {
  json_object *node = NULL, *ue = NULL, *bits = NULL;
  if (!json_object_is_type(obj, json_type_object) || json_object_object_length(obj) != 2 ||
      !json_object_object_get_ex(obj, "node", &node) || !json_object_object_get_ex(obj, "ue", &ue) ||
      !json_object_is_type(node, json_type_object) || json_object_object_length(node) != 6 ||
      !json_object_is_type(ue, json_type_object) || json_object_object_length(ue) != 2) return false;
  const char *mcc = bk_string(node, "mcc"), *mnc = bk_string(node, "mnc");
  const char *nt = bk_string(node, "node_type"), *ut = bk_string(ue, "id_type");
  if (!digits(mcc,3,3) || !digits(mnc,2,3) || !nt || strcmp(nt,"ngran_gNB_DU") || !ut || strcmp(ut,"gnb_du") ||
      !json_object_object_get_ex(node,"gnb_id_bits",&bits) || !json_object_is_type(bits,json_type_int)) return false;
  int64_t nbits = json_object_get_int64(bits); uint64_t gnb, du, f1ap;
  if (nbits < 22 || nbits > 32 || !bk_uint(node,"gnb_id",(UINT64_C(1)<<nbits)-1,&gnb) ||
      !bk_uint(node,"du_id",(UINT64_C(1)<<36)-1,&du) || !bk_uint(ue,"gnb_cu_ue_f1ap_id",UINT32_MAX,&f1ap)) return false;
  memset(key,0,sizeof(*key)); strcpy(key->mcc,mcc); strcpy(key->mnc,mnc);
  key->bits = nbits; key->gnb = gnb; key->du = du; key->ue = f1ap; return true;
}
void bk_add_u64(json_object *obj, const char *name, uint64_t value) {
  char s[21]; snprintf(s,sizeof(s),"%" PRIu64,value); json_object_object_add(obj,name,json_object_new_string(s));
}
json_object *bk_node_json(const bk_key *key) {
  json_object *obj = json_object_new_object();
  json_object_object_add(obj,"mcc",json_object_new_string(key->mcc));
  json_object_object_add(obj,"mnc",json_object_new_string(key->mnc));
  json_object_object_add(obj,"node_type",json_object_new_string("ngran_gNB_DU"));
  bk_add_u64(obj,"gnb_id",key->gnb); bk_add_u64(obj,"du_id",key->du);
  json_object_object_add(obj,"gnb_id_bits",json_object_new_int(key->bits)); return obj;
}
json_object *bk_ue_json(const bk_row *row) {
  json_object *obj = json_object_new_object();
  json_object_object_add(obj,"id_type",json_object_new_string("gnb_du"));
  bk_add_u64(obj,"gnb_cu_ue_f1ap_id",row->key.ue);
  if (row->has_ran_ue) bk_add_u64(obj,"ran_ue_id",row->ran_ue);
  else json_object_object_add(obj,"ran_ue_id",NULL);
  return obj;
}
json_object *bk_names_json(void) {
  json_object *obj = json_object_new_array();
  for (unsigned i = 0; i < BK_METRICS; ++i) json_object_array_add(obj,json_object_new_string(bk_names[i]));
  return obj;
}
json_object *bk_envelope(const bk_context *ctx, const char *type) {
  json_object *obj = json_object_new_object();
  json_object_object_add(obj,"version",json_object_new_int(1));
  json_object_object_add(obj,"type",json_object_new_string(type));
  json_object_object_add(obj,"broker_instance_id",json_object_new_string(ctx->instance));
  json_object_object_add(obj,"data_source",json_object_new_string(ctx->source));
  json_object_object_add(obj,"metric_profile",json_object_new_string("base5")); return obj;
}
json_object *bk_row_json(const bk_context *ctx, const bk_row *row) {
  static const char *types[] = {"absent","integer","real","noValue","unsupported"};
  static const char *states[] = {"ok","not_reported","no_value","invalid"};
  json_object *obj = bk_envelope(ctx,"kpm"), *metrics = json_object_new_object();
  bk_add_u64(obj,"report_seq",row->report_seq); bk_add_u64(obj,"header_time_us",row->header_us);
  bk_add_u64(obj,"received_at_unix_us",row->received_us); bk_add_u64(obj,"published_at_unix_us",bk_unix_us());
  bk_add_u64(obj,"ingress_dropped_total",atomic_load(&ctx->dropped));
  json_object_object_add(obj,"sample_idx",json_object_new_int(row->sample_idx));
  json_object_object_add(obj,"incomplete",row->incomplete < 0 ? NULL : json_object_new_boolean(row->incomplete));
  json_object_object_add(obj,"node",bk_node_json(&row->key)); json_object_object_add(obj,"ue",bk_ue_json(row));
  for (unsigned i = 0; i < BK_METRICS; ++i) {
    const bk_value *v = &row->values[i]; json_object *m = json_object_new_object(), *number = NULL;
    json_object_object_add(m,"kpm_type",json_object_new_string(types[v->type]));
    json_object_object_add(m,"status",json_object_new_string(states[v->status]));
    if (v->status == BK_OK) number = v->type == BK_INTEGER ? json_object_new_int64(v->integer) : json_object_new_double(v->real);
    json_object_object_add(m,"value",number); json_object_object_add(m,"unit",json_object_new_string(bk_units[i]));
    if (v->status == BK_INVALID) json_object_object_add(m,"reason",json_object_new_string(v->type == BK_REAL ? "non_finite" : "unsupported_type"));
    json_object_object_add(metrics,bk_names[i],m);
  }
  json_object_object_add(obj,"metrics",metrics); return obj;
}
/* Independent evidence representation: no network JSON encoder or rounded REALs. */
void bk_trace(bk_context *ctx, const bk_row *row) {
  if (!ctx->evidence) return;
  if (ctx->evidence_count >= ctx->cfg.evidence_limit) { ++ctx->evidence_limited; return; }
  FILE *f = ctx->evidence;
  fprintf(f,"{\"instance\":\"%s\",\"profile\":\"base5\",\"report_seq\":\"%" PRIu64
    "\",\"mcc\":\"%s\",\"mnc\":\"%s\",\"gnb\":\"%u\",\"bits\":%u,\"du\":\"%" PRIu64
    "\",\"ue\":\"%u\",\"sample_idx\":%u,\"header_us\":\"%" PRIu64 "\",\"received_us\":\"%" PRIu64
    "\",\"incomplete\":%d,\"ran_ue\":",ctx->instance,row->report_seq,row->key.mcc,row->key.mnc,row->key.gnb,
    row->key.bits,row->key.du,row->key.ue,row->sample_idx,row->header_us,row->received_us,row->incomplete);
  if (row->has_ran_ue) fprintf(f,"\"%" PRIu64 "\"",row->ran_ue); else fputs("null",f);
  fputs(",\"values\":[",f);
  for (unsigned i = 0; i < BK_METRICS; ++i) {
    const bk_value *v = &row->values[i]; uint64_t bits; memcpy(&bits,&v->real,sizeof(bits));
    fprintf(f,"%s{\"type\":%d,\"state\":%d,\"integer\":%u,\"real_bits\":\"%016" PRIx64 "\"}",
      i ? "," : "",v->type,v->status,v->integer,bits);
  }
  fputs("]}\n",f); ++ctx->evidence_count;
  if (fflush(f)) atomic_store(&ctx->failed,true);
}
json_object *bk_parse(const char *bytes, size_t len) {
  if (!len || len > BK_INPUT || memchr(bytes,0,len)) return NULL;
  json_tokener *tok = json_tokener_new_ex(16); if (!tok) return NULL;
  json_tokener_set_flags(tok,JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  json_object *obj = json_tokener_parse_ex(tok,bytes,(int)len);
  bool valid = json_tokener_get_error(tok) == json_tokener_success && json_object_is_type(obj,json_type_object);
  size_t end = json_tokener_get_parse_end(tok);
  while (end < len && isspace((unsigned char)bytes[end])) ++end;
  valid = valid && end == len; json_tokener_free(tok);
  if (!valid) { json_object_put(obj); return NULL; } return obj;
}
void bk_defaults(bk_config *c) {
  *c = (bk_config){.port=8765,.clients=16,.ingress_rows=256,.ingress_bytes=4*1024*1024,
    .output_frames=64,.output_bytes=1024*1024,.frame_bytes=16384,.registration_ms=5000,
    .incomplete_ms=5000,.idle_ms=30000,.write_ms=5000,.drain_ms=2000,.evidence_limit=1000};
}
bool bk_config_load(bk_config *c, const char *path) {
  FILE *f = fopen(path,"rb"); if (!f) return false;
  char bytes[BK_INPUT+1]; size_t len = fread(bytes,1,sizeof(bytes),f); bool io = !ferror(f); fclose(f);
  json_object *obj = io ? bk_parse(bytes,len) : NULL; if (!obj) return false;
  bool ok = true;
  json_object_object_foreach(obj,name,value) {
    if (!strcmp(name,"test_stall_app")) {
      const char *s=bk_string(obj,name);
      if (!bk_identifier(s)) ok=false; else strcpy(c->test_stall_app,s);
      continue;
    }
    if (!strcmp(name,"bind")) {
      const char *s = bk_string(obj,name); if (!s || strcmp(s,"127.0.0.1")) ok=false;
      continue;
    }
    if (!json_object_is_type(value,json_type_int)) { ok=false; continue; }
    int64_t n = json_object_get_int64(value); bool known = false;
#define SET(field,low,high) if (!strcmp(name,#field)) { known=true; if (n < (int64_t)(low) || (uint64_t)n > (high)) ok=false; else c->field=n; }
    SET(port,1,65535) SET(clients,1,BK_CLIENTS) SET(ingress_rows,1,BK_ROWS)
    SET(ingress_bytes,sizeof(bk_row),4*1024*1024) SET(output_frames,1,BK_OUTPUT)
    SET(output_bytes,128,1024*1024) SET(frame_bytes,128,BK_FRAME)
    SET(registration_ms,20,5000) SET(incomplete_ms,20,5000) SET(idle_ms,20,30000)
    SET(write_ms,20,5000) SET(drain_ms,20,2000) SET(write_chunk,0,BK_FRAME)
    SET(evidence_limit,1,10000000)
#undef SET
    if (!known) ok=false;
  }
  json_object_put(obj); return ok;
}
