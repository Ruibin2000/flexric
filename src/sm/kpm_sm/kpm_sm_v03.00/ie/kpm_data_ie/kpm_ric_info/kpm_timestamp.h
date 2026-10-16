#ifndef KPM_V3_TIMESTAMP_H
#define KPM_V3_TIMESTAMP_H

#include <assert.h>
#include <stdint.h>

/* KPM v3 uses an eight-octet, big-endian NTP 32.32 timestamp on the wire.
 * FlexRIC's internal collectStartTime remains Unix microseconds.
 */
static inline uint64_t kpm_timestamp_read_be(const uint8_t bytes[8])
{
  uint64_t timestamp = 0;
  for (unsigned i = 0; i < 8; ++i)
    timestamp = (timestamp << 8) | bytes[i];
  return timestamp;
}

static inline void kpm_timestamp_write_be(uint8_t bytes[8], uint64_t timestamp)
{
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = (uint8_t)(timestamp >> (56 - 8 * i));
}

static inline uint64_t kpm_timestamp_from_unix_us(uint64_t unix_us)
{
  const uint64_t seconds = unix_us / UINT64_C(1000000);
  const uint64_t microseconds = unix_us % UINT64_C(1000000);
  const uint64_t ntp_seconds = (seconds + UINT64_C(2208988800)) & UINT64_C(0xffffffff);
  /* Round up so conversion back to whole microseconds preserves the input. */
  const uint64_t fraction = ((microseconds << 32) + UINT64_C(999999)) / UINT64_C(1000000);
  return (ntp_seconds << 32) | fraction;
}

static inline uint64_t kpm_timestamp_to_unix_us(uint64_t timestamp, int64_t reference_unix_seconds)
{
  const int64_t ntp_era_seconds = INT64_C(4294967296);
  const uint32_t reference_ntp_seconds = (uint32_t)(reference_unix_seconds + INT64_C(2208988800));
  int64_t delta = (int64_t)(timestamp >> 32) - reference_ntp_seconds;
  /* Select the NTP era nearest receipt time, including the 2036 rollover.
   * The timestamp must be within 2^31 seconds (about 68 years) of reference.
   */
  if (delta > INT32_MAX)
    delta -= ntp_era_seconds;
  else if (delta < INT32_MIN)
    delta += ntp_era_seconds;
  const int64_t unix_seconds = reference_unix_seconds + delta;
  assert(unix_seconds >= 0 && "Pre-1970 timestamps cannot use the unsigned Unix-us API");
  const uint64_t microseconds = ((timestamp & UINT64_C(0xffffffff)) * UINT64_C(1000000)) >> 32;
  return (uint64_t)unix_seconds * UINT64_C(1000000) + microseconds;
}

#endif
