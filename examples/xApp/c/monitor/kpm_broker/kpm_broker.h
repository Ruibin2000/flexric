#ifndef FALCON_KPM_BROKER_H
#define FALCON_KPM_BROKER_H
#include "xApp/e42_xapp_api.h"
#include <json-c/json.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdio.h>

#define BK_METRICS 5
#define BK_ROWS 256
#define BK_CLIENTS 16
#define BK_FILTERS 32
#define BK_INPUT 8192
#define BK_FRAME 16384
#define BK_OUTPUT 64
extern const char *const bk_names[BK_METRICS], *const bk_units[BK_METRICS];
typedef struct {
  char mcc[4], mnc[4];
  uint32_t gnb, ue;
  unsigned bits;
  uint64_t du;
} bk_key;
typedef struct {
  enum { BK_ABSENT, BK_INTEGER, BK_REAL, BK_NO_VALUE, BK_UNSUPPORTED } type;
  enum { BK_OK, BK_NOT_REPORTED, BK_NO_DATA, BK_INVALID } status;
  uint32_t integer;
  double real;
} bk_value;
typedef struct {
  bk_key key;
  uint64_t report_seq, header_us, received_us, ran_ue;
  bool has_ran_ue;
  unsigned sample_idx;
  int incomplete; /* -1 absent, 0 false, 1 true */
  bk_value values[BK_METRICS];
} bk_row;
typedef struct {
  unsigned port, clients, ingress_rows, output_frames;
  size_t ingress_bytes, output_bytes, frame_bytes;
  unsigned registration_ms, incomplete_ms, idle_ms, write_ms, drain_ms;
  unsigned write_chunk; /* 0: normal; deterministic partial-I/O verification */
  unsigned evidence_limit;
  char test_stall_app[65]; /* Fixture-only I/O adapter; rejected in live mode. */
} bk_config;
typedef struct {
  bk_config cfg;
  pthread_mutex_t mutex;
  bk_row rows[BK_ROWS];
  size_t head, count, peak;
  bool accepting;
  atomic_uint_fast64_t sequence, dropped, malformed;
  atomic_bool stop, ready, failed, admission_closed;
  int wake;
  const char *source;
  char instance[65];
  FILE *evidence;
  unsigned evidence_count;
  uint64_t evidence_limited, serialization_errors, admission_rejects, client_overflows;
} bk_context;

uint64_t bk_mono_ms(void);
uint64_t bk_unix_us(void);
void bk_defaults(bk_config *cfg);
bool bk_config_load(bk_config *cfg, const char *path);
bool bk_init(bk_context *ctx, const bk_config *cfg, const char *source, const char *evidence);
void bk_destroy(bk_context *ctx);
void bk_disable(bk_context *ctx);
void bk_wake(bk_context *ctx);
void bk_callback(bk_context *ctx, const sm_ag_if_rd_t *rd, const global_e2_node_id_t *node);
bool bk_pop(bk_context *ctx, bk_row *row);
void bk_enqueue_owned(bk_context *ctx, const bk_row *row);
bool bk_key_equal(const bk_key *a, const bk_key *b);
bool bk_key_parse(json_object *obj, bk_key *key);
json_object *bk_node_json(const bk_key *key);
json_object *bk_ue_json(const bk_row *row);
json_object *bk_names_json(void);
json_object *bk_envelope(const bk_context *ctx, const char *type);
json_object *bk_row_json(const bk_context *ctx, const bk_row *row);
void bk_trace(bk_context *ctx, const bk_row *row);
json_object *bk_parse(const char *bytes, size_t len);
const char *bk_string(json_object *obj, const char *name);
bool bk_identifier(const char *s);
bool bk_uint(json_object *obj, const char *name, uint64_t max, uint64_t *value);
void bk_add_u64(json_object *obj, const char *name, uint64_t value);
void *bk_server(void *arg);
bool bk_fixture_load(bk_context *ctx, const char *dir, const char *plugin, unsigned period_ms,
                     bool repeat, volatile sig_atomic_t *stopping);
bool bk_source_run(bk_context *ctx, int argc, char **argv, volatile sig_atomic_t *stopping);
bool bk_source_check(int argc, char **argv);
#endif
