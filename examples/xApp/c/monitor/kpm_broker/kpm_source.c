/* Style 4 builders retained from the reviewed Falcon identity collector. */
#include "kpm_broker.h"
#include "sm/kpm_sm/kpm_sm_id_wrapper.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static
kpm_event_trigger_def_t gen_kpm_ev_trig(uint64_t period)
{
  kpm_event_trigger_def_t dst = {0};

  dst.type = FORMAT_1_RIC_EVENT_TRIGGER;
  dst.kpm_ric_event_trigger_format_1.report_period_ms = period;

  return dst;
}

static
meas_info_format_1_lst_t gen_meas_info_format_1_lst(const act_name_id_t act)
{
  meas_info_format_1_lst_t dst = {0};

  // use id
  if (!strcasecmp(act.name, "null")) {
    dst.meas_type.type = ID_MEAS_TYPE;
    dst.meas_type.id = act.id;
  } else { // use name
    dst.meas_type.type = NAME_MEAS_TYPE;
    // ETSI TS 128 552
    dst.meas_type.name = cp_str_to_ba(act.name);
  }

  dst.label_info_lst_len = 1;
  dst.label_info_lst = calloc(1, sizeof(label_info_lst_t));
  assert(dst.label_info_lst != NULL && "Memory exhausted");

  // No Label
  dst.label_info_lst[0].noLabel = calloc(1, sizeof(enum_value_e));
  assert(dst.label_info_lst[0].noLabel != NULL && "Memory exhausted");
  *dst.label_info_lst[0].noLabel = TRUE_ENUM_VALUE;

  return dst;
}

static
kpm_act_def_format_1_t gen_kpm_act_def_frmt_1(const sub_oran_sm_t sub_sm, uint32_t period_ms)
{
  kpm_act_def_format_1_t dst = {0};

  dst.gran_period_ms = period_ms;

  dst.meas_info_lst_len = sub_sm.act_len;
  dst.meas_info_lst = calloc(dst.meas_info_lst_len, sizeof(meas_info_format_1_lst_t));
  assert(dst.meas_info_lst != NULL && "Memory exhausted");

  for(size_t i = 0; i < dst.meas_info_lst_len; i++) {
    dst.meas_info_lst[i] = gen_meas_info_format_1_lst(sub_sm.actions[i]);
  }

  return dst;
}

static
kpm_act_def_format_4_t gen_kpm_act_def_frmt_4(const sub_oran_sm_t sub_sm, uint32_t period_ms)
{
  kpm_act_def_format_4_t dst = {0};

  // [1, 32768]
  dst.matching_cond_lst_len = 1;

  dst.matching_cond_lst = calloc(dst.matching_cond_lst_len, sizeof(matching_condition_format_4_lst_t));
  assert(dst.matching_cond_lst != NULL && "Memory exhausted");

  test_info_lst_t* test_info_lst = &dst.matching_cond_lst[0].test_info_lst;
  test_info_lst->test_cond_type = S_NSSAI_TEST_COND_TYPE;
  test_info_lst->S_NSSAI = TRUE_TEST_COND_TYPE;

  test_cond_e* test_cond = calloc(1, sizeof(test_cond_e));
  assert(test_cond != NULL && "Memory exhausted");
  *test_cond = EQUAL_TEST_COND;
  test_info_lst->test_cond = test_cond;

  test_cond_value_t* test_cond_value = calloc(1, sizeof(test_cond_value_t));
  assert(test_cond_value != NULL && "Memory exhausted");
  test_cond_value->type = INTEGER_TEST_COND_VALUE;
  test_cond_value->int_value = calloc(1, sizeof(int64_t));
  assert(test_cond_value->int_value != NULL && "Memory exhausted");
  *test_cond_value->int_value = 1;
  test_info_lst->test_cond_value = test_cond_value;

  // Action definition Format 1
  dst.action_def_format_1 = gen_kpm_act_def_frmt_1(sub_sm, period_ms);  // 8.2.1.2.1

  return dst;
}

static
kpm_act_def_t gen_kpm_act_def(const sub_oran_sm_t sub_sm, format_action_def_e act_frm, uint32_t period_ms)
{
  kpm_act_def_t dst = {0};

  if (act_frm == FORMAT_1_ACTION_DEFINITION) {
    dst.type = FORMAT_1_ACTION_DEFINITION;
    dst.frm_1 = gen_kpm_act_def_frmt_1(sub_sm, period_ms);
  } else if (act_frm == FORMAT_4_ACTION_DEFINITION) {
    dst.type = FORMAT_4_ACTION_DEFINITION;
    dst.frm_4 = gen_kpm_act_def_frmt_4(sub_sm, period_ms);
  } else {
    assert(0!=0 && "not support action definition type");
  }

  return dst;
}



static bk_context *callback_context;
static void source_callback(const sm_ag_if_rd_t *rd, const global_e2_node_id_t *node) {
  bk_callback(callback_context,rd,node);
}
static bool capability(const e2_node_connected_xapp_t *node) {
  for (size_t f=0;f<node->len_rf;++f) {
    const sm_ran_function_t *rf=&node->rf[f];
    if (rf->id!=SM_KPM_ID || rf->defn.type!=KPM_RAN_FUNC_DEF_E) continue;
    const kpm_ran_function_def_t *def=&rf->defn.kpm;
    for (size_t s=0;s<def->sz_ric_report_style_list;++s) {
      const ric_report_style_item_t *style=&def->ric_report_style_list[s];
      if (style->act_def_format_type!=FORMAT_4_ACTION_DEFINITION ||
          style->ind_msg_format_type!=FORMAT_3_INDICATION_MESSAGE) continue;
      unsigned found=0;
      for (unsigned k=0;k<BK_METRICS;++k) {
        bool present=false;
        for (size_t m=0;m<style->meas_info_for_action_lst_len;++m) {
          const byte_array_t *name=&style->meas_info_for_action_lst[m].name;
          if (name->buf && name->len==strlen(bk_names[k]) && !memcmp(name->buf,bk_names[k],name->len)) present=true;
        }
        if (present) ++found;
      }
      if (found==BK_METRICS) return true;
    }
  }
  return false;
}
static bool valid_config(const fr_args_t *args) {
  bool valid=args->sub_oran_sm_len==1 && args->sub_cust_sm_len==0 && !args->db_params.enable;
  const sub_oran_sm_t *spec=&args->sub_oran_sm[0];
  valid=valid && spec->name && !strcmp(spec->name,"KPM") && spec->ran_type &&
    !strcmp(spec->ran_type,"ngran_gNB_DU") && spec->format==4 && spec->time==1000 && spec->act_len==BK_METRICS;
  for (unsigned k=0;valid && k<BK_METRICS;++k)
    valid=spec->actions[k].name && !strcmp(spec->actions[k].name,bk_names[k]);
  return valid;
}
bool bk_source_check(int argc, char **argv) {
  fr_args_t args=init_fr_args(argc,argv); bool valid=valid_config(&args);
  if (valid) {
    kpm_act_def_t action=gen_kpm_act_def(args.sub_oran_sm[0],FORMAT_4_ACTION_DEFINITION,1000);
    const kpm_act_def_format_1_t *group=&action.frm_4.action_def_format_1;
    valid=group->meas_info_lst_len==BK_METRICS && group->gran_period_ms==1000;
    for (unsigned i=0;valid && i<BK_METRICS;++i) {
      const meas_info_format_1_lst_t *metric=&group->meas_info_lst[i];
      valid=metric->meas_type.type==NAME_MEAS_TYPE && metric->meas_type.name.len==strlen(bk_names[i]) &&
        !memcmp(metric->meas_type.name.buf,bk_names[i],strlen(bk_names[i])) && metric->label_info_lst_len==1 &&
        metric->label_info_lst[0].noLabel && *metric->label_info_lst[0].noLabel==TRUE_ENUM_VALUE;
    }
    free_kpm_action_def(&action);
  }
  free_fr_args(&args);
  if (valid) puts("BASE5_CONFIG_VALID style=4 reporting_ms=1000 granularity_ms=1000 metrics=5");
  return valid;
}
bool bk_source_run(bk_context *ctx, int argc, char **argv, volatile sig_atomic_t *stopping) {
  fr_args_t args=init_fr_args(argc,argv);
  bool valid=valid_config(&args);
  const sub_oran_sm_t *spec=&args.sub_oran_sm[0];
  if (!valid) { fprintf(stderr,"base5 requires exactly five DU Style 4 actions at 1000 ms, database OFF\n"); free_fr_args(&args); return false; }
  callback_context=ctx;
  struct sigaction before_int, before_term;
  sigaction(SIGINT,NULL,&before_int); sigaction(SIGTERM,NULL,&before_term);
  init_xapp_api(&args);
  /* The SDK replaces SIGINT with an abrupt shutdown handler. Restore flags. */
  sigaction(SIGINT,&before_int,NULL); sigaction(SIGTERM,&before_term,NULL);
  e2_node_arr_xapp_t nodes=e2_nodes_xapp_api();
  int handles[256]; size_t count=0; bool success=true;
  for (size_t n=0;n<nodes.len && !*stopping;++n) {
    e2_node_connected_xapp_t *node=&nodes.n[n];
    if (node->id.type!=e2ap_ngran_gNB_DU) continue;
    if (!node->id.cu_du_id || !capability(node)) { success=false; break; }
    bool duplicate=false;
    for (size_t j=0;j<n;++j) if (eq_global_e2_node_id(&nodes.n[j].id,&node->id)) duplicate=true;
    if (duplicate) { success=false; break; }
    kpm_sub_data_t sub={0}; sub.ev_trg_def=gen_kpm_ev_trig(1000); sub.sz_ad=1;
    sub.ad=calloc(1,sizeof(*sub.ad)); assert(sub.ad);
    sub.ad[0]=gen_kpm_act_def(*spec,FORMAT_4_ACTION_DEFINITION,1000);
    sm_ans_xapp_t answer=report_sm_xapp_api(&node->id,SM_KPM_ID,&sub,source_callback);
    free_kpm_sub_data(&sub);
    if (!answer.success) { success=false; break; }
    handles[count++]=answer.u.handle;
  }
  if (!count) success=false;
  if (success) printf("E2_READY shared_du_subscriptions=%zu profile=base5 period_ms=1000\n",count);
  else fprintf(stderr,"E2 source unavailable or base5 capability/subscription rejected\n");
  while (success && !*stopping && !atomic_load(&ctx->failed)) usleep(10000);
  bk_disable(ctx);
  for (size_t i=0;i<count;++i) rm_report_sm_xapp_api(handles[i]);
  free_e2_node_arr_xapp(&nodes);
  while (!try_stop_xapp_api()) usleep(1000);
  callback_context=NULL; free_fr_args(&args); return success;
}
