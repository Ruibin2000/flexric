#define _GNU_SOURCE
#include "kpm_broker.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct { char *bytes; size_t len, offset; } frame;
typedef struct {
  int fd;
  enum { AWAITING, REGISTERED, CLOSING } state;
  char app[65], subscription[32], input[BK_INPUT];
  size_t input_len, filter_len;
  bk_key filters[BK_FILTERS];
  bool all;
  uint64_t cutoff, delivery, opened, activity, partial_since, progress, closing_since, matched, sent_frames;
  frame output[BK_OUTPUT];
  size_t head, count, bytes;
} client;

static void remove_client(client *c, const char *reason) {
  if (c->fd < 0) return;
  fprintf(stderr,"CLIENT_CLOSE app=%s reason=%s\n",c->app,reason);
  close(c->fd);
  for (size_t j = 0; j < c->count; ++j) free(c->output[(c->head+j)%BK_OUTPUT].bytes);
  memset(c,0,sizeof(*c)); c->fd=-1;
}
static bool queue_json(bk_context *ctx, client *c, json_object *obj) {
  size_t len = 0; const char *s = json_object_to_json_string_length(obj,JSON_C_TO_STRING_PLAIN,&len);
  if (!s || len+1 > ctx->cfg.frame_bytes) {
    ++ctx->serialization_errors; remove_client(c,"serialization_limit"); return false;
  }
  if (c->count >= ctx->cfg.output_frames || len+1 > ctx->cfg.output_bytes-c->bytes) {
    ++ctx->client_overflows; remove_client(c,"output_overflow"); return false;
  }
  char *owned = malloc(len+1);
  if (!owned) { atomic_store(&ctx->failed,true); remove_client(c,"allocation_failure"); return false; }
  memcpy(owned,s,len); owned[len]='\n';
  c->output[(c->head+c->count)%BK_OUTPUT] = (frame){.bytes=owned,.len=len+1};
  if (!c->count) c->progress=bk_mono_ms();
  ++c->count; c->bytes+=len+1; return true;
}
static void closing(client *c) {
  if (c->state!=CLOSING) { c->state=CLOSING; c->closing_since=bk_mono_ms(); }
}
static void reject(bk_context *ctx, client *c, const char *type, const char *request, const char *code, bool fatal) {
  ++ctx->admission_rejects;
  json_object *obj = bk_envelope(ctx,type);
  if (request) json_object_object_add(obj,"request_id",json_object_new_string(request));
  json_object_object_add(obj,"status",json_object_new_string("rejected"));
  json_object_object_add(obj,"error_code",json_object_new_string(code));
  if (queue_json(ctx,c,obj) && fatal) closing(c);
  json_object_put(obj);
}
static bool version_one(json_object *obj) {
  json_object *v=NULL;
  return json_object_object_get_ex(obj,"version",&v) && json_object_is_type(v,json_type_int) && json_object_get_int64(v)==1;
}
static bool filter_parse(json_object *obj, bk_key keys[BK_FILTERS], size_t *len, bool *all) {
  if (!json_object_is_type(obj,json_type_object)) return false;
  const char *mode=bk_string(obj,"mode"); if (!mode) return false;
  *len=0; *all=false;
  if (!strcmp(mode,"all_du_ues")) { *all=true; return json_object_object_length(obj)==1; }
  json_object *links=NULL;
  if (strcmp(mode,"links") || json_object_object_length(obj)!=2 || !json_object_object_get_ex(obj,"links",&links) ||
      !json_object_is_type(links,json_type_array)) return false;
  *len=json_object_array_length(links); if (!*len || *len>BK_FILTERS) return false;
  for (size_t j=0;j<*len;++j) {
    if (!bk_key_parse(json_object_array_get_idx(links,j),&keys[j])) return false;
    for (size_t z=0;z<j;++z) if (bk_key_equal(&keys[j],&keys[z])) return false;
  }
  return true;
}
static void data_state(json_object *obj, const client *c) {
  json_object_object_add(obj,"data_state",json_object_new_string(c->matched ? "receiving_kpm" : "waiting_for_kpm"));
  json_object_object_add(obj,"last_matching_kpm_age_ms",c->matched ? json_object_new_int64(bk_mono_ms()-c->matched) : NULL);
}
static void request(bk_context *ctx, client clients[BK_CLIENTS], client *c, const char *s, size_t len, uint64_t *next_sub) {
  json_object *obj=bk_parse(s,len);
  const char *id=bk_string(obj,"request_id"), *type=bk_string(obj,"type");
  if (!bk_identifier(id)) {
    reject(ctx,c,"error",NULL,"invalid_request",true); json_object_put(obj); return;
  }
  bool registration=type && !strcmp(type,"register");
  const char *response=registration ? "registration_status" : "error";
  if (!version_one(obj)) { reject(ctx,c,response,id,"unsupported_version",false); goto done; }
  if (!type) { reject(ctx,c,response,id,"invalid_request",false); goto done; }
  if (registration) {
    if (atomic_load(&ctx->admission_closed)) { reject(ctx,c,response,id,"shutting_down",true); goto done; }
    if (c->state==REGISTERED) { reject(ctx,c,response,id,"already_registered",false); goto done; }
    const char *app=bk_string(obj,"app_id"); json_object *filter=NULL;
    bk_key keys[BK_FILTERS]; size_t count; bool all;
    if (json_object_object_length(obj)!=5 || !bk_identifier(app) || !json_object_object_get_ex(obj,"filter",&filter) ||
        !filter_parse(filter,keys,&count,&all)) { reject(ctx,c,response,id,"invalid_request",false); goto done; }
    for (unsigned i=0;i<ctx->cfg.clients;++i) if (clients[i].fd>=0 && clients[i].state==REGISTERED && !strcmp(clients[i].app,app)) {
      reject(ctx,c,response,id,"duplicate_app_id",false); goto done;
    }
    /* Worker commits cutoff before queueing status. Callback sequences are atomic. */
    c->cutoff=atomic_load(&ctx->sequence); c->state=REGISTERED; strcpy(c->app,app);
    snprintf(c->subscription,sizeof(c->subscription),"sub-%" PRIu64,++*next_sub);
    c->all=all; c->filter_len=count; memcpy(c->filters,keys,count*sizeof(*keys));
    json_object *reply=bk_envelope(ctx,response);
    json_object_object_add(reply,"request_id",json_object_new_string(id));
    json_object_object_add(reply,"status",json_object_new_string("registered"));
    json_object_object_add(reply,"app_id",json_object_new_string(c->app));
    json_object_object_add(reply,"subscription_id",json_object_new_string(c->subscription));
    json_object_object_add(reply,"metric_names",bk_names_json());
    json_object_object_add(reply,"report_period_ms",json_object_new_int(1000));
    json_object_object_add(reply,"delivery",json_object_new_string("best_effort_no_replay"));
    bk_add_u64(reply,"registration_cutoff_report_seq",c->cutoff); data_state(reply,c);
    queue_json(ctx,c,reply); json_object_put(reply);
  } else if (!strcmp(type,"ping") && json_object_object_length(obj)==3) {
    json_object *reply=bk_envelope(ctx,"pong");
    json_object_object_add(reply,"request_id",json_object_new_string(id)); data_state(reply,c);
    queue_json(ctx,c,reply); json_object_put(reply);
  } else if (!strcmp(type,"unregister") && c->state==REGISTERED && json_object_object_length(obj)==4) {
    const char *sub=bk_string(obj,"subscription_id");
    if (!sub || strcmp(sub,c->subscription)) { reject(ctx,c,"error",id,"invalid_request",false); goto done; }
    json_object *reply=bk_envelope(ctx,"unregistration_status");
    json_object_object_add(reply,"request_id",json_object_new_string(id));
    json_object_object_add(reply,"subscription_id",json_object_new_string(c->subscription));
    json_object_object_add(reply,"status",json_object_new_string("unregistered"));
    if (queue_json(ctx,c,reply)) closing(c);
    json_object_put(reply);
  } else { reject(ctx,c,response,id,"invalid_request",false); goto done; }
  c->activity=bk_mono_ms();
done:
  json_object_put(obj);
}
static void receive(bk_context *ctx, client clients[BK_CLIENTS], client *c, uint64_t *next_sub) {
  /* Bound work per client per poll iteration to prevent request flooding starvation. */
  char chunk[4096]; ssize_t n=recv(c->fd,chunk,sizeof(chunk),0);
  if (!n) { remove_client(c,"eof"); return; }
  if (n<0) { if (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) remove_client(c,"read_error"); return; }
  for (ssize_t i=0;i<n && c->fd>=0 && c->state!=CLOSING;++i) {
    if (chunk[i]=='\n') {
      request(ctx,clients,c,c->input,c->input_len,next_sub); c->input_len=0; c->partial_since=0;
    } else if (c->input_len==BK_INPUT) {
      reject(ctx,c,"error",NULL,"frame_too_large",true);
    } else {
      if (!c->input_len) c->partial_since=bk_mono_ms();
      c->input[c->input_len++]=chunk[i];
    }
  }
}
static void transmit(bk_context *ctx, client *c) {
  if (ctx->cfg.test_stall_app[0] && !strcmp(ctx->cfg.test_stall_app,c->app) && c->sent_frames) return;
  frame *f=&c->output[c->head]; size_t remaining=f->len-f->offset;
  if (ctx->cfg.write_chunk && remaining>ctx->cfg.write_chunk) remaining=ctx->cfg.write_chunk;
  ssize_t n=send(c->fd,f->bytes+f->offset,remaining,MSG_NOSIGNAL);
  if (n>0) {
    f->offset+=n; c->progress=bk_mono_ms();
    if (f->offset==f->len) { c->bytes-=f->len; free(f->bytes); *f=(frame){0}; c->head=(c->head+1)%BK_OUTPUT; --c->count; ++c->sent_frames; }
  } else if (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) remove_client(c,"write_error");
}
static bool matches(const client *c, const bk_row *row) {
  if (c->state!=REGISTERED || row->report_seq<=c->cutoff) return false;
  if (c->all) return true;
  for (size_t i=0;i<c->filter_len;++i) if (bk_key_equal(&c->filters[i],&row->key)) return true;
  return false;
}
void *bk_server(void *arg) {
  bk_context *ctx=arg; client clients[BK_CLIENTS];
  memset(clients,0,sizeof(clients)); for (unsigned i=0;i<BK_CLIENTS;++i) clients[i].fd=-1;
  int listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
  int yes=1; if (listener>=0) setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(ctx->cfg.port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  if (listener<0 || bind(listener,(struct sockaddr*)&address,sizeof(address)) || listen(listener,16)) {
    perror("broker listener"); if (listener>=0) close(listener); atomic_store(&ctx->failed,true); return NULL;
  }
  atomic_store(&ctx->ready,true);
  printf("BROKER_READY bind=127.0.0.1 port=%u source=%s profile=base5 instance=%s\n",ctx->cfg.port,ctx->source,ctx->instance);
  uint64_t next_sub=0, shutdown_at=0;
  for (;;) {
    uint64_t now=bk_mono_ms();
    if (listener>=0 && atomic_load(&ctx->admission_closed)) { close(listener); listener=-1; }
    if (atomic_load(&ctx->stop) && !shutdown_at) {
      shutdown_at=now; if (listener>=0) close(listener); listener=-1;
      for (unsigned i=0;i<ctx->cfg.clients;++i) if (clients[i].fd>=0) closing(&clients[i]);
    }
    struct pollfd fds[BK_CLIENTS+2]={{.fd=listener,.events=POLLIN},{.fd=ctx->wake,.events=POLLIN}};
    for (unsigned i=0;i<ctx->cfg.clients;++i) {
      client *c=&clients[i]; fds[i+2]=(struct pollfd){.fd=c->fd,
        .events=(c->state==CLOSING ? 0 : POLLIN) | (c->count ? POLLOUT : 0)};
    }
    int rc=poll(fds,ctx->cfg.clients+2,20);
    if (rc<0 && errno!=EINTR) { atomic_store(&ctx->failed,true); break; }
    if (fds[1].revents&POLLIN) { uint64_t value; ssize_t n=read(ctx->wake,&value,sizeof(value)); (void)n; }
    if (listener>=0 && (fds[0].revents&POLLIN)) {
      int fd=accept4(listener,NULL,NULL,SOCK_NONBLOCK|SOCK_CLOEXEC);
      if (fd>=0) {
        unsigned i=0; while (i<ctx->cfg.clients && clients[i].fd>=0) ++i;
        if (i==ctx->cfg.clients) {
          ++ctx->admission_rejects;
          const char rejection[]="{\"version\":1,\"type\":\"registration_status\",\"status\":\"rejected\",\"error_code\":\"too_many_clients\"}\n";
          ssize_t n=send(fd,rejection,sizeof(rejection)-1,MSG_NOSIGNAL); (void)n; close(fd);
        } else { clients[i].fd=fd; clients[i].opened=clients[i].activity=bk_mono_ms(); }
      } else if (errno!=EAGAIN && errno!=EINTR) { atomic_store(&ctx->failed,true); break; }
    }
    for (unsigned i=0;i<ctx->cfg.clients;++i) {
      client *c=&clients[i]; short ev=fds[i+2].revents;
      /* A newly admitted client did not have this slot in the poll snapshot. */
      if (c->fd<0 || c->fd!=fds[i+2].fd) continue;
      if (ev&(POLLERR|POLLNVAL)) { remove_client(c,"socket_error"); continue; }
      if ((ev&POLLIN) && c->state!=CLOSING) receive(ctx,clients,c,&next_sub);
      if (c->fd>=0 && c->count && (ev&POLLOUT)) transmit(ctx,c);
      if (c->fd>=0 && (ev&POLLHUP) && !(ev&POLLIN)) remove_client(c,"hangup");
    }
    /* Bounded batch; return to sockets even under continuous ingress. */
    bk_row row;
    for (unsigned batch=0;batch<64 && bk_pop(ctx,&row);++batch) {
      bk_trace(ctx,&row);
      for (unsigned i=0;i<ctx->cfg.clients;++i) {
        client *c=&clients[i]; if (c->fd<0 || !matches(c,&row)) continue;
        json_object *obj=bk_row_json(ctx,&row);
        json_object_object_add(obj,"subscription_id",json_object_new_string(c->subscription));
        bk_add_u64(obj,"delivery_seq",++c->delivery);
        if (queue_json(ctx,c,obj)) c->matched=bk_mono_ms();
        json_object_put(obj);
      }
    }
    now=bk_mono_ms(); bool active=false;
    for (unsigned i=0;i<ctx->cfg.clients;++i) {
      client *c=&clients[i]; if (c->fd<0) continue;
      const char *reason=NULL;
      if (c->state==CLOSING && (!c->count || now-c->closing_since>=ctx->cfg.drain_ms)) reason="drained_or_drain_timeout";
      else if (c->state==AWAITING && now-c->opened>=ctx->cfg.registration_ms) reason="registration_timeout";
      else if (c->partial_since && now-c->partial_since>=ctx->cfg.incomplete_ms) reason="incomplete_timeout";
      else if (c->state==REGISTERED && now-c->activity>=ctx->cfg.idle_ms) reason="idle_timeout";
      else if (c->count && now-c->progress>=ctx->cfg.write_ms) reason="write_timeout";
      if (reason) remove_client(c,reason); else active=true;
    }
    if (shutdown_at && (!active || now-shutdown_at>=ctx->cfg.drain_ms)) break;
    if (atomic_load(&ctx->failed)) break;
  }
  for (unsigned i=0;i<ctx->cfg.clients;++i) remove_client(&clients[i],"shutdown");
  if (listener>=0) close(listener);
  pthread_mutex_lock(&ctx->mutex); size_t ingress_peak=ctx->peak; pthread_mutex_unlock(&ctx->mutex);
  fprintf(stderr,"BROKER_STATS ingress_dropped=%" PRIuFAST64 " malformed=%" PRIuFAST64
    " ingress_peak=%zu serialization_errors=%" PRIu64 " admission_rejects=%" PRIu64
    " client_overflows=%" PRIu64 " evidence_rows=%u evidence_limited=%" PRIu64 "\n",
    atomic_load(&ctx->dropped),atomic_load(&ctx->malformed),ingress_peak,ctx->serialization_errors,
    ctx->admission_rejects,ctx->client_overflows,ctx->evidence_count,ctx->evidence_limited);
  return NULL;
}
