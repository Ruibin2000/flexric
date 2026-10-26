#define _GNU_SOURCE
#include "kpm_broker.h"
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

const char *const bk_names[BK_METRICS] = {
  "DRB.RlcSduDelayDl", "DRB.UEThpDl", "DRB.UEThpUl", "RRU.PrbTotDl", "RRU.PrbTotUl"};
const char *const bk_units[BK_METRICS] = {"0.1 ms", "kbit/s", "kbit/s", "%", "%"};
uint64_t bk_mono_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
uint64_t bk_unix_us(void) {
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
bool bk_init(bk_context *ctx, const bk_config *cfg, const char *source, const char *evidence) {
  memset(ctx, 0, sizeof(*ctx)); ctx->cfg = *cfg; ctx->source = source; ctx->wake = -1;
  atomic_init(&ctx->sequence, 0); atomic_init(&ctx->dropped, 0); atomic_init(&ctx->malformed, 0);
  atomic_init(&ctx->stop, false); atomic_init(&ctx->ready, false); atomic_init(&ctx->failed, false);
  atomic_init(&ctx->admission_closed, false);
  if (pthread_mutex_init(&ctx->mutex, NULL)) return false;
  ctx->wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  unsigned char random[16];
  if (ctx->wake < 0 || getrandom(random, sizeof(random), 0) != sizeof(random)) goto fail;
  for (size_t i = 0; i < sizeof(random); ++i) snprintf(ctx->instance + i*2, 3, "%02x", random[i]);
  if (evidence && !(ctx->evidence = fopen(evidence, "wx"))) goto fail;
  ctx->accepting = true;
  return true;
fail:
  if (ctx->wake >= 0) close(ctx->wake);
  pthread_mutex_destroy(&ctx->mutex);
  return false;
}
void bk_wake(bk_context *ctx) {
  uint64_t one = 1;
  ssize_t n = write(ctx->wake, &one, sizeof(one));
  if (n < 0 && errno != EAGAIN && errno != EINTR) atomic_store(&ctx->failed, true);
}
void bk_disable(bk_context *ctx) {
  pthread_mutex_lock(&ctx->mutex); ctx->accepting = false; pthread_mutex_unlock(&ctx->mutex);
  atomic_store(&ctx->admission_closed,true); bk_wake(ctx);
}
void bk_destroy(bk_context *ctx) {
  if (ctx->evidence && fclose(ctx->evidence)) atomic_store(&ctx->failed, true);
  close(ctx->wake); pthread_mutex_destroy(&ctx->mutex);
}
bool bk_key_equal(const bk_key *a, const bk_key *b) {
  return !strcmp(a->mcc, b->mcc) && !strcmp(a->mnc, b->mnc) && a->gnb == b->gnb &&
    a->bits == b->bits && a->du == b->du && a->ue == b->ue;
}
bool bk_pop(bk_context *ctx, bk_row *row) {
  pthread_mutex_lock(&ctx->mutex);
  bool found = ctx->count != 0;
  if (found) { *row = ctx->rows[ctx->head]; ctx->head = (ctx->head + 1) % BK_ROWS; --ctx->count; }
  pthread_mutex_unlock(&ctx->mutex); return found;
}
void bk_enqueue_owned(bk_context *ctx, const bk_row *row) {
  pthread_mutex_lock(&ctx->mutex);
  if (ctx->accepting) {
    if (ctx->count >= ctx->cfg.ingress_rows || (ctx->count + 1)*sizeof(*row) > ctx->cfg.ingress_bytes)
      atomic_fetch_add(&ctx->dropped, 1);
    else {
      ctx->rows[(ctx->head + ctx->count) % BK_ROWS] = *row;
      if (++ctx->count > ctx->peak) ctx->peak = ctx->count;
    }
  }
  pthread_mutex_unlock(&ctx->mutex); bk_wake(ctx);
}
static bool node_key(const global_e2_node_id_t *node, bk_key *key) {
  if (!node || node->type != e2ap_ngran_gNB_DU || !node->cu_du_id || node->nb_id.unused > 10 ||
      node->plmn.mcc > 999 || (node->plmn.mnc_digit_len != 2 && node->plmn.mnc_digit_len != 3) ||
      node->plmn.mnc > (node->plmn.mnc_digit_len == 2 ? 99 : 999)) return false;
  key->bits = 32 - node->nb_id.unused;
  if ((uint64_t)node->nb_id.nb_id >= (UINT64_C(1) << key->bits) || *node->cu_du_id >= (UINT64_C(1)<<36)) return false;
  unsigned mcc=node->plmn.mcc, mnc=node->plmn.mnc, digits=node->plmn.mnc_digit_len;
  for (int i=2;i>=0;--i) { key->mcc[i]='0'+mcc%10; mcc/=10; }
  for (int i=(int)digits-1;i>=0;--i) { key->mnc[i]='0'+mnc%10; mnc/=10; }
  key->mcc[3]=0; key->mnc[digits]=0;
  key->gnb = node->nb_id.nb_id; key->du = *node->cu_du_id; return true;
}
static bool mapping(const kpm_ind_msg_format_1_t *rows, int indices[BK_METRICS]) {
  if (!rows->meas_info_lst_len || rows->meas_info_lst_len > 64 || !rows->meas_info_lst ||
      !rows->meas_data_lst_len || rows->meas_data_lst_len > 256 || !rows->meas_data_lst) return false;
  for (unsigned k = 0; k < BK_METRICS; ++k) indices[k] = -1;
  for (size_t j = 0; j < rows->meas_info_lst_len; ++j) {
    const meas_type_t *type = &rows->meas_info_lst[j].meas_type;
    if (type->type != NAME_MEAS_TYPE || !type->name.buf || !type->name.len || type->name.len > 150) return false;
    for (size_t z = 0; z < j; ++z) {
      const byte_array_t *previous = &rows->meas_info_lst[z].meas_type.name;
      if (previous->len == type->name.len && !memcmp(previous->buf, type->name.buf, previous->len)) return false;
    }
    for (unsigned k = 0; k < BK_METRICS; ++k)
      if (strlen(bk_names[k]) == type->name.len && !memcmp(bk_names[k], type->name.buf, type->name.len)) indices[k] = (int)j;
  }
  for (size_t j = 0; j < rows->meas_data_lst_len; ++j) {
    const meas_data_lst_t *row = &rows->meas_data_lst[j];
    if (row->meas_record_len != rows->meas_info_lst_len || !row->meas_record_lst ||
        (row->incomplete_flag && *row->incomplete_flag != TRUE_ENUM_VALUE)) return false;
  }
  return true;
}
void bk_callback(bk_context *ctx, const sm_ag_if_rd_t *rd, const global_e2_node_id_t *node) {
  /* Sequence is captured before normalization, including callbacks later rejected. */
  uint64_t seq = atomic_fetch_add(&ctx->sequence, 1) + 1, received = bk_unix_us();
  bk_key key = {0};
  if (!rd || rd->type != INDICATION_MSG_AGENT_IF_ANS_V0 || rd->ind.type != KPM_STATS_V3_0 || !node_key(node, &key)) goto bad;
  const kpm_ind_data_t *kpm = &rd->ind.kpm.ind;
  if (kpm->hdr.type != FORMAT_1_INDICATION_HEADER || kpm->msg.type != FORMAT_3_INDICATION_MESSAGE) goto bad;
  const kpm_ind_msg_format_3_t *msg = &kpm->msg.frm_3;
  if (!msg->ue_meas_report_lst_len || msg->ue_meas_report_lst_len > 256 || !msg->meas_report_per_ue) goto bad;
  /* Validate the whole indication before publishing any rows. */
  for (size_t u = 0; u < msg->ue_meas_report_lst_len; ++u) {
    const meas_report_per_ue_t *ue = &msg->meas_report_per_ue[u]; int indices[BK_METRICS];
    if (ue->ue_meas_report_lst.type != GNB_DU_UE_ID_E2SM || !mapping(&ue->ind_msg_format_1, indices)) goto bad;
    for (size_t z = 0; z < u; ++z)
      if (msg->meas_report_per_ue[z].ue_meas_report_lst.gnb_du.gnb_cu_ue_f1ap == ue->ue_meas_report_lst.gnb_du.gnb_cu_ue_f1ap) goto bad;
  }
  for (size_t u = 0; u < msg->ue_meas_report_lst_len; ++u) {
    const meas_report_per_ue_t *ue = &msg->meas_report_per_ue[u];
    const kpm_ind_msg_format_1_t *rows = &ue->ind_msg_format_1; int indices[BK_METRICS];
    mapping(rows, indices);
    for (size_t j = 0; j < rows->meas_data_lst_len; ++j) {
      bk_row row = {.key=key, .report_seq=seq, .received_us=received,
        .header_us=kpm->hdr.kpm_ric_ind_hdr_format_1.collectStartTime, .sample_idx=j, .incomplete=-1};
      row.key.ue = ue->ue_meas_report_lst.gnb_du.gnb_cu_ue_f1ap;
      if (ue->ue_meas_report_lst.gnb_du.ran_ue_id) {
        row.has_ran_ue = true; row.ran_ue = *ue->ue_meas_report_lst.gnb_du.ran_ue_id;
      }
      if (rows->meas_data_lst[j].incomplete_flag) row.incomplete = 1;
      for (unsigned k = 0; k < BK_METRICS; ++k) {
        bk_value *v = &row.values[k]; v->type = BK_ABSENT; v->status = BK_NOT_REPORTED;
        if (indices[k] < 0) continue;
        const meas_record_lst_t *r = &rows->meas_data_lst[j].meas_record_lst[indices[k]];
        v->status = BK_OK;
        if (r->value == INTEGER_MEAS_VALUE) { v->type = BK_INTEGER; v->integer = r->int_val; }
        else if (r->value == REAL_MEAS_VALUE) {
          v->type = BK_REAL; v->real = r->real_val;
          if (!isfinite(v->real)) v->status = BK_INVALID;
        } else if (r->value == NO_VALUE_MEAS_VALUE) { v->type = BK_NO_VALUE; v->status = BK_NO_DATA; }
        else { v->type = BK_UNSUPPORTED; v->status = BK_INVALID; }
      }
      bk_enqueue_owned(ctx, &row);
    }
  }
  return;
bad:
  atomic_fetch_add(&ctx->malformed, 1); /* Worker emits diagnostics; callback performs no I/O. */
}
