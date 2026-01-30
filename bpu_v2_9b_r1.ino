/*
  BPU v2.9b-r1 (Dual UART demo) — FINAL (cleanup + safety)

  Streams:
    - LOG: Serial  @115200 (human-readable)
    - OUT: Serial1 @921600 (binary frames)

  ESP32-WROOM pins:
    - Serial1 TX: GPIO17
    - Serial1 RX: GPIO16 (configured but not used)
*/

#include <Arduino.h>
#include <stdarg.h>
#include <stdint.h>

// -----------------------------------------------------------------------------
// Streams
// -----------------------------------------------------------------------------
static HardwareSerial& LOG = Serial;
static HardwareSerial& OUT = Serial1;

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------
static const uint32_t LOG_BAUD = 115200;
static const uint32_t OUT_BAUD = 921600;

static const int OUT_TX_PIN = 17;
static const int OUT_RX_PIN = 16;   // configured but not used

static const uint32_t TICK_MS = 20;

static const uint32_t SENSOR_MS = 80;
static const uint32_t HB_MS     = 200;
static const uint32_t TELEM_MS  = 1000;

static const uint32_t COALESCE_WINDOW_MS = 20;
static const uint32_t AGED_MS = 200;

static const uint16_t TX_BUDGET_BYTES = 200;
static const bool ENABLE_DEGRADE = true;
static const bool DEBUG_DUMP_TX_HEX = false;

// Minimum free bytes required to attempt sending a frame.
static const int OUT_MIN_FREE = 96;

// -----------------------------------------------------------------------------
// Types
// -----------------------------------------------------------------------------
enum : uint8_t {
  EVT_CMD    = 1,
  EVT_SENSOR = 2,
  EVT_HB     = 3,
  EVT_TELEM  = 4,
};

enum : uint8_t {
  JOB_CMD    = 1,
  JOB_SENSOR = 2,
  JOB_HB     = 3,
  JOB_TELEM  = 4,
};

enum MergePolicy : uint8_t {
  MERGE_NONE = 0,
  MERGE_LAST = 1,
};

static inline MergePolicy policy_for(uint8_t type){
  switch(type){
    case EVT_SENSOR: return MERGE_LAST;
    case EVT_HB:     return MERGE_LAST;
    case EVT_TELEM:  return MERGE_LAST;
    default:         return MERGE_NONE;
  }
}

static inline uint8_t job_for_evt(uint8_t evt_type){
  switch(evt_type){
    case EVT_CMD:    return JOB_CMD;
    case EVT_SENSOR: return JOB_SENSOR;
    case EVT_HB:     return JOB_HB;
    case EVT_TELEM:  return JOB_TELEM;
    default:         return 0;
  }
}

static inline uint64_t bit64(uint8_t n){ return (n >= 64) ? 0ULL : (1ULL << n); }

struct BpuEvent {
  uint8_t  type;
  uint8_t  flags;
  uint16_t len;
  uint32_t t_ms;
  uint8_t  payload[16];
};

struct BpuJob {
  uint8_t  type;
  uint8_t  flags;
  uint16_t len;
  uint32_t t_ms;
  uint8_t  payload[32];
};

struct BpuStats {
  uint32_t tick = 0;
  uint32_t ev_in=0, ev_out=0, ev_merge=0, ev_drop=0;
  uint32_t job_in=0, job_out=0, job_merge=0, job_drop=0;

  uint32_t uart_sent=0, uart_skip_budget=0, uart_skip_txbuf=0, uart_bytes=0;
  uint32_t flush_try=0, flush_ok=0, flush_partial=0, flush_full=0;

  uint32_t pick_cmd=0, pick_sensor=0, pick_hb=0, pick_telem=0, pick_aged=0;
  uint32_t aged_hit_sensor=0, aged_hit_hb=0, aged_hit_telem=0;

  uint32_t degrade_drop=0, degrade_requeue=0;

  uint32_t work_us_last=0, work_us_max=0;

  uint32_t out_bytes_total=0;
  uint32_t log_bytes_total=0;
};

template<typename T, size_t N>
struct Ring {
  T buf[N];
  uint16_t head = 0, tail = 0, count = 0;

  bool push(const T& v){
    if(count >= N) return false;
    buf[head] = v;
    head = (head + 1) % N;
    count++;
    return true;
  }
  bool pop(T& out){
    if(count == 0) return false;
    out = buf[tail];
    tail = (tail + 1) % N;
    count--;
    return true;
  }
  T& at(size_t idx){ return buf[(tail + idx) % N]; }
};

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
static void     logf(const char* fmt, ...);
static uint16_t crc16_ccitt(const uint8_t* data, size_t len);
static size_t   cobs_encode(const uint8_t* input, size_t length, uint8_t* output, size_t out_max);

static bool evq_push_coalesce(const BpuEvent& e);
static bool evq_pop(BpuEvent& out);

static bool jobq_push_coalesce(const BpuJob& j);
static bool jobq_pop(BpuJob& out);

static uint64_t dirty_derived();
static bool uart_send_frame(uint8_t type, const uint8_t* payload, uint8_t len, uint16_t& bytes_sent_out);

static void schedule_sources(uint32_t now_ms);
static void schedule_from_events(uint32_t now_ms);

static bool flush_one(uint32_t now_ms, uint16_t& budget_left);
static void bpu_tick(uint32_t now_ms);

// -----------------------------------------------------------------------------
// Globals
// -----------------------------------------------------------------------------
static BpuStats st;

static const size_t EVT_QN = 8;
static const size_t JOB_QN = 4;

static Ring<BpuEvent, EVT_QN> evq;
static Ring<BpuJob,   JOB_QN> jobq;

static uint8_t  g_seq = 0;
static uint32_t t_next_sensor=0, t_next_hb=0, t_next_telem=0;

// -----------------------------------------------------------------------------
// Logging (counts bytes written to LOG)
// -----------------------------------------------------------------------------
static void logf(const char* fmt, ...){
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if(n <= 0) return;
  if(n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;

  LOG.print(buf);
  st.log_bytes_total += (uint32_t)n;
}

// -----------------------------------------------------------------------------
// CRC16-CCITT
// -----------------------------------------------------------------------------
static uint16_t crc16_ccitt(const uint8_t* data, size_t len){
  uint16_t crc = 0xFFFF;
  for(size_t i=0;i<len;i++){
    crc ^= (uint16_t)data[i] << 8;
    for(int b=0;b<8;b++){
      if(crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else            crc = (crc << 1);
    }
  }
  return crc;
}

// -----------------------------------------------------------------------------
// COBS encode
// -----------------------------------------------------------------------------
static size_t cobs_encode(const uint8_t* input, size_t length, uint8_t* output, size_t out_max){
  if(out_max == 0) return 0;
  size_t read_index = 0;
  size_t write_index = 1;
  size_t code_index = 0;
  uint8_t code = 1;

  while(read_index < length){
    if(write_index >= out_max) return 0;
    if(input[read_index] == 0){
      output[code_index] = code;
      code = 1;
      code_index = write_index++;
      read_index++;
    } else {
      output[write_index++] = input[read_index++];
      code++;
      if(code == 0xFF){
        output[code_index] = code;
        code = 1;
        code_index = write_index++;
      }
    }
  }
  if(code_index >= out_max) return 0;
  output[code_index] = code;
  return write_index;
}

// -----------------------------------------------------------------------------
// Derived dirty mask: job types currently queued
// -----------------------------------------------------------------------------
static uint64_t dirty_derived(){
  uint64_t m = 0;
  for(size_t i=0;i<jobq.count;i++){
    const BpuJob& j = jobq.at(i);
    if(j.type >= 1 && j.type <= 63) m |= bit64(j.type);
  }
  return m;
}

// -----------------------------------------------------------------------------
// Event queue (optional coalescing by type)
// -----------------------------------------------------------------------------
static bool evq_push_coalesce(const BpuEvent& e){
  st.ev_in++;

  if(COALESCE_WINDOW_MS > 0 && policy_for(e.type) == MERGE_LAST && evq.count > 0){
    for(size_t i=0;i<evq.count;i++){
      BpuEvent& ex = evq.at(i);
      if(ex.type == e.type){
        if((uint32_t)(e.t_ms - ex.t_ms) <= COALESCE_WINDOW_MS){
          ex = e;
          st.ev_merge++;
          return true;
        }
      }
    }
  }

  if(!evq.push(e)){
    st.ev_drop++;
    return false;
  }
  return true;
}

static bool evq_pop(BpuEvent& out){
  if(!evq.pop(out)) return false;
  st.ev_out++;
  return true;
}

// -----------------------------------------------------------------------------
// Job queue (coalesce by type; keep last)
// -----------------------------------------------------------------------------
static bool jobq_push_coalesce(const BpuJob& j){
  st.job_in++;

  for(size_t i=0;i<jobq.count;i++){
    BpuJob& ex = jobq.at(i);
    if(ex.type == j.type){
      ex = j;
      st.job_merge++;
      return true;
    }
  }

  if(!jobq.push(j)){
    st.job_drop++;
    return false;
  }
  return true;
}

static bool jobq_pop(BpuJob& out){
  if(!jobq.pop(out)) return false;
  st.job_out++;
  return true;
}

// -----------------------------------------------------------------------------
// OUT frame: [0xB2, type, seq, len, payload..., crc16] -> COBS -> 0x00 delimiter
// -----------------------------------------------------------------------------
static bool uart_send_frame(uint8_t type, const uint8_t* payload, uint8_t len, uint16_t& bytes_sent_out){
  if(len > 64) return false;

  uint8_t decoded[4 + 64 + 2];
  decoded[0] = 0xB2;
  decoded[1] = type;
  decoded[2] = g_seq++;
  decoded[3] = len;

  for(uint8_t i=0;i<len;i++) decoded[4+i] = payload[i];

  // CRC covers: type, seq, len, payload...
  uint16_t crc = crc16_ccitt(&decoded[1], (size_t)(3 + len));
  decoded[4+len+0] = (uint8_t)(crc & 0xFF);
  decoded[4+len+1] = (uint8_t)((crc >> 8) & 0xFF);

  const size_t decoded_len = (size_t)(4 + len + 2);

  uint8_t encoded[4 + 64 + 2 + 16];
  size_t enc_len = cobs_encode(decoded, decoded_len, encoded, sizeof(encoded));
  if(enc_len == 0) return false;

  bytes_sent_out = (uint16_t)(enc_len + 1); // + delimiter

  OUT.write(encoded, enc_len);
  OUT.write((uint8_t)0x00);

  st.out_bytes_total += bytes_sent_out;

  if(DEBUG_DUMP_TX_HEX){
    logf("TX ");
    for(size_t i=0;i<enc_len;i++){
      if(encoded[i] < 16) logf("0");
      logf("%02X ", encoded[i]);
    }
    logf("00\n");
  }

  return true;
}

// -----------------------------------------------------------------------------
// Demo sources: generate SENSOR/HB/TELEM events
// -----------------------------------------------------------------------------
static void schedule_sources(uint32_t now_ms){
  if((int32_t)(now_ms - t_next_sensor) >= 0){
    t_next_sensor = now_ms + SENSOR_MS;

    BpuEvent e{};
    e.type = EVT_SENSOR;
    e.flags = 0;
    e.t_ms = now_ms;

    uint16_t v = (uint16_t)((now_ms / 10) & 0xFFFF);
    e.len = 2;
    e.payload[0] = (uint8_t)(v & 0xFF);
    e.payload[1] = (uint8_t)((v >> 8) & 0xFF);

    st.pick_sensor++;
    evq_push_coalesce(e);
  }

  if((int32_t)(now_ms - t_next_hb) >= 0){
    t_next_hb = now_ms + HB_MS;

    BpuEvent e{};
    e.type = EVT_HB;
    e.flags = 0;
    e.t_ms = now_ms;
    e.len = 1;
    e.payload[0] = 0x01;

    st.pick_hb++;
    evq_push_coalesce(e);
  }

  if((int32_t)(now_ms - t_next_telem) >= 0){
    t_next_telem = now_ms + TELEM_MS;

    BpuEvent e{};
    e.type = EVT_TELEM;
    e.flags = 0;
    e.t_ms = now_ms;

    e.len = 4;
    uint32_t m = now_ms;
    e.payload[0] = (uint8_t)(m & 0xFF);
    e.payload[1] = (uint8_t)((m >> 8) & 0xFF);
    e.payload[2] = (uint8_t)((m >> 16) & 0xFF);
    e.payload[3] = (uint8_t)((m >> 24) & 0xFF);

    st.pick_telem++;
    evq_push_coalesce(e);
  }
}

// -----------------------------------------------------------------------------
// Events -> Jobs
// -----------------------------------------------------------------------------
static void schedule_from_events(uint32_t now_ms){
  BpuEvent e{};
  while(evq_pop(e)){
    const bool aged = (uint32_t)(now_ms - e.t_ms) >= AGED_MS;
    if(aged){
      st.pick_aged++;
      if(e.type == EVT_SENSOR) st.aged_hit_sensor++;
      if(e.type == EVT_HB)     st.aged_hit_hb++;
      if(e.type == EVT_TELEM)  st.aged_hit_telem++;
    }

    BpuJob j{};
    j.type  = job_for_evt(e.type);
    j.flags = e.flags;
    j.t_ms  = now_ms;

    uint8_t tag = 0;
    if(e.type == EVT_SENSOR) tag = 0x01;
    if(e.type == EVT_HB)     tag = 0x02;
    if(e.type == EVT_TELEM)  tag = 0x03;
    if(e.type == EVT_CMD)    tag = 0x04;

    j.payload[0] = tag;
    j.payload[1] = (uint8_t)e.len;

    uint16_t copy_n = e.len;
    if(copy_n > (sizeof(j.payload) - 2)) copy_n = (uint16_t)(sizeof(j.payload) - 2);
    for(uint16_t i=0;i<copy_n;i++) j.payload[2+i] = e.payload[i];

    j.len = (uint16_t)(2 + copy_n);
    jobq_push_coalesce(j);
  }
}

// -----------------------------------------------------------------------------
// Flush one job with budget + TX backpressure handling
// -----------------------------------------------------------------------------
static bool flush_one(uint32_t now_ms, uint16_t& budget_left){
  (void)now_ms;
  st.flush_try++;

  if(jobq.count == 0) return false;

  BpuJob j{};
  if(!jobq_pop(j)) return false;

  // Worst-case on-wire estimate (COBS overhead + delimiter)
  const size_t decoded_len     = 4 + j.len + 2;
  const size_t worst_overhead  = (decoded_len / 254) + 2;
  const size_t worst_on_wire   = decoded_len + worst_overhead + 1;

  if(worst_on_wire > budget_left){
    st.uart_skip_budget++;

    if(ENABLE_DEGRADE){
      // Prefer dropping TELEM over blocking higher-priority streams.
      if(j.type == JOB_TELEM){
        st.degrade_drop++;
        return false;
      }
      // Requeue others (coalescing keeps last)
      (void)jobq_push_coalesce(j);
      st.degrade_requeue++;
      return false;
    }

    (void)jobq_push_coalesce(j);
    return false;
  }

  if(OUT.availableForWrite() < OUT_MIN_FREE){
    st.uart_skip_txbuf++;
    (void)jobq_push_coalesce(j);
    st.degrade_requeue++;
    return false;
  }

  // Safety clamp: j.len is uint16_t; wire len must be uint8_t.
  const uint8_t wire_len = (j.len > 255) ? 255 : (uint8_t)j.len;

  uint16_t sent_bytes = 0;
  if(!uart_send_frame(j.type, j.payload, wire_len, sent_bytes)){
    (void)jobq_push_coalesce(j);
    st.degrade_requeue++;
    return false;
  }

  budget_left = (uint16_t)(budget_left - sent_bytes);
  st.uart_sent++;
  st.uart_bytes += sent_bytes;
  st.flush_ok++;

  return true;
}

// -----------------------------------------------------------------------------
// Tick
// -----------------------------------------------------------------------------
static void bpu_tick(uint32_t now_ms){
  const uint32_t t0 = (uint32_t)micros();

  schedule_sources(now_ms);
  schedule_from_events(now_ms);

  bool sent_any = false;
  uint16_t budget = TX_BUDGET_BYTES;

  while(budget > 0 && jobq.count > 0){
    const uint16_t before = budget;
    if(!flush_one(now_ms, budget)){
      // Stop if budget didn't move to avoid spinning.
      if(before == budget) break;
    } else {
      sent_any = true;
    }
  }

  // Per-tick flush outcome
  if(sent_any){
    if(jobq.count == 0) st.flush_full++;
    else               st.flush_partial++;
  }

  st.tick++;

  const uint32_t t1 = (uint32_t)micros();
  const uint32_t work_us = (t1 >= t0) ? (t1 - t0) : 0;
  st.work_us_last = work_us;
  if(work_us > st.work_us_max) st.work_us_max = work_us;

  static uint32_t last_print_ms = 0;
  if((int32_t)(now_ms - last_print_ms) >= 200){
    last_print_ms = now_ms;

    const uint64_t dirty = dirty_derived();

    logf(
      "[BPU2.9b-r1] tick=%lu ev(in/out/merge/drop)=%lu/%lu/%lu/%lu evQ=%u "
      "job(in/out/merge/drop)=%lu/%lu/%lu/%lu jobQ=%u dirty=0x%016llX "
      "uart(sent/skipB/skipTX/bytes)=%lu/%lu/%lu/%lu "
      "flush(try/ok/partial/full)=%lu/%lu/%lu/%lu "
      "pick(sensor/hb/telem/aged)=%lu/%lu/%lu/%lu aged_hit(s/h/t)=%lu/%lu/%lu "
      "degrade(drop/requeue)=%lu/%lu work_us(last/max)=%lu/%lu "
      "streams(OUT/LOG)=%lu/%luB\n",
      (unsigned long)st.tick,
      (unsigned long)st.ev_in, (unsigned long)st.ev_out, (unsigned long)st.ev_merge, (unsigned long)st.ev_drop,
      (unsigned)evq.count,
      (unsigned long)st.job_in, (unsigned long)st.job_out, (unsigned long)st.job_merge, (unsigned long)st.job_drop,
      (unsigned)jobq.count,
      (unsigned long long)dirty,
      (unsigned long)st.uart_sent, (unsigned long)st.uart_skip_budget, (unsigned long)st.uart_skip_txbuf, (unsigned long)st.uart_bytes,
      (unsigned long)st.flush_try, (unsigned long)st.flush_ok, (unsigned long)st.flush_partial, (unsigned long)st.flush_full,
      (unsigned long)st.pick_sensor, (unsigned long)st.pick_hb, (unsigned long)st.pick_telem, (unsigned long)st.pick_aged,
      (unsigned long)st.aged_hit_sensor, (unsigned long)st.aged_hit_hb, (unsigned long)st.aged_hit_telem,
      (unsigned long)st.degrade_drop, (unsigned long)st.degrade_requeue,
      (unsigned long)st.work_us_last, (unsigned long)st.work_us_max,
      (unsigned long)st.out_bytes_total, (unsigned long)st.log_bytes_total
    );
  }
}

// -----------------------------------------------------------------------------
// Arduino entry points
// -----------------------------------------------------------------------------
void setup(){
  LOG.begin(LOG_BAUD);
  delay(200);

  // RX pin is configured but not used; TX pin is active for OUT stream.
  OUT.begin(OUT_BAUD, SERIAL_8N1, OUT_RX_PIN, OUT_TX_PIN);
  delay(50);

  logf("\n");
  logf("BPU v2.9b-r1 boot\n");
  logf("LOG: Serial @%lu\n", (unsigned long)LOG_BAUD);
  logf("OUT: Serial1 TX=GPIO%d @%lu\n", OUT_TX_PIN, (unsigned long)OUT_BAUD);

  const uint32_t now = millis();
  t_next_sensor = now + 10;
  t_next_hb     = now + 50;
  t_next_telem  = now + 200;
}

void loop(){
  static uint32_t last_tick_ms = 0;
  const uint32_t now = millis();

  // Catch up if loop stalls (prevents silent tick loss).
  while((int32_t)(now - last_tick_ms) >= (int32_t)TICK_MS){
    last_tick_ms += TICK_MS;
    bpu_tick(now);
  }

  delay(1);
}
