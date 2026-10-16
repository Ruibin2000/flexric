#include <assert.h>
#include "../../../../../util/conversions.h"
#include <time.h>

#include "dec_ric_ind_hdr_frm_1.h"
#include "../../ie/kpm_data_ie/kpm_ric_info/kpm_timestamp.h"

kpm_ric_ind_hdr_format_1_t kpm_dec_ind_hdr_frm_1_asn(const E2SM_KPM_IndicationHeader_Format1_t * ind_hdr_asn)
{
    kpm_ric_ind_hdr_format_1_t ind_hdr = {0};

    assert(ind_hdr_asn != NULL);
    assert(ind_hdr_asn->colletStartTime.size == 8);
    assert(ind_hdr_asn->colletStartTime.buf != NULL);
    const time_t reference = time(NULL);
    assert(reference != (time_t)-1 && "Unable to read timestamp era reference");
    const uint64_t ntp_timestamp = kpm_timestamp_read_be(ind_hdr_asn->colletStartTime.buf);
    ind_hdr.collectStartTime = kpm_timestamp_to_unix_us(ntp_timestamp, (int64_t)reference);
    
    if (ind_hdr_asn->fileFormatversion != NULL){
      ind_hdr.fileformat_version = calloc(1, sizeof(byte_array_t));
      ind_hdr.fileformat_version->buf = calloc(ind_hdr_asn->fileFormatversion->size + 1, sizeof(uint8_t));
      assert(ind_hdr.fileformat_version->buf != NULL && "Memory exhausted" );
      memcpy(ind_hdr.fileformat_version->buf, ind_hdr_asn->fileFormatversion->buf, ind_hdr_asn->fileFormatversion->size);
      ind_hdr.fileformat_version->len = ind_hdr_asn->fileFormatversion->size;
    }

    if (ind_hdr_asn->senderName != NULL){
      ind_hdr.sender_name = calloc(1, sizeof(byte_array_t));
      ind_hdr.sender_name->buf = calloc(ind_hdr_asn->senderName->size + 1, sizeof(uint8_t));
      assert(ind_hdr.sender_name->buf != NULL && "Memory exhausted" );
      memcpy(ind_hdr.sender_name->buf, ind_hdr_asn->senderName->buf, ind_hdr_asn->senderName->size);
      ind_hdr.sender_name->len = ind_hdr_asn->senderName->size;
    }

    if (ind_hdr_asn->senderType != NULL){
      ind_hdr.sender_type = calloc(1, sizeof(byte_array_t));
      ind_hdr.sender_type->buf = calloc(ind_hdr_asn->senderType->size + 1, sizeof(uint8_t));
      assert(ind_hdr.sender_type->buf != NULL && "Memory exhausted" );
      memcpy(ind_hdr.sender_type->buf, ind_hdr_asn->senderType->buf, ind_hdr_asn->senderType->size);
      ind_hdr.sender_type->len = ind_hdr_asn->senderType->size;
    }

    if (ind_hdr_asn->vendorName != NULL){
      ind_hdr.vendor_name = calloc(1, sizeof(byte_array_t));
      ind_hdr.vendor_name->buf = calloc(ind_hdr_asn->vendorName->size + 1, sizeof(uint8_t));
      assert(ind_hdr.vendor_name->buf != NULL && "Memory exhausted" );
      memcpy(ind_hdr.vendor_name->buf, ind_hdr_asn->vendorName->buf, ind_hdr_asn->vendorName->size);
      ind_hdr.vendor_name->len = ind_hdr_asn->vendorName->size;
    }


    return ind_hdr;
}
