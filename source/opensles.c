/* opensles.c -- see opensles.h */
#include <math.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "log.h"
#include "opensles.h"
#include "util.h"

#define SL_RESULT_SUCCESS              0x00
#define SL_RESULT_PARAMETER_INVALID    0x02
#define SL_RESULT_MEMORY_FAILURE       0x03
#define SL_RESULT_BUFFER_INSUFFICIENT  0x07
#define SL_RESULT_FEATURE_UNSUPPORTED  0x0C

#define SL_OBJECT_STATE_REALIZED 2
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED  2
#define SL_PLAYSTATE_PLAYING 3
#define SL_DATAFORMAT_PCM    2

#define DEV_RATE     48000
#define DEV_FRAMES   960
#define DEV_BUFFERS  4
#define MAX_PLAYERS  32
#define BQ_SLOTS     64

#define DEF_IID(name) static char iid_##name; void *SL_IID_##name = &iid_##name
DEF_IID(NULL); DEF_IID(OBJECT); DEF_IID(ENGINE); DEF_IID(OUTPUTMIX); DEF_IID(PLAY);
DEF_IID(BUFFERQUEUE); DEF_IID(ANDROIDSIMPLEBUFFERQUEUE); DEF_IID(VOLUME); DEF_IID(PLAYBACKRATE);
DEF_IID(SEEK); DEF_IID(EFFECTSEND); DEF_IID(PREFETCHSTATUS); DEF_IID(ANDROIDCONFIGURATION);
DEF_IID(METADATAEXTRACTION); DEF_IID(ENVIRONMENTALREVERB);

#define CONTAINER(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

typedef uint32_t SLresult;

struct ObjectItf {
  SLresult (*Realize)(void *self, uint32_t async);
  SLresult (*Resume)(void *self, uint32_t async);
  SLresult (*GetState)(void *self, uint32_t *state);
  SLresult (*GetInterface)(void *self, const void *iid, void *out);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  void     (*AbortAsyncOperation)(void *self);
  void     (*Destroy)(void *self);
  SLresult (*SetPriority)(void *self, int32_t prio, uint32_t preempt);
  SLresult (*GetPriority)(void *self, int32_t *prio, uint32_t *preempt);
  SLresult (*SetLossOfControlInterfaces)(void *self, int16_t n, void *iids, uint32_t enabled);
};

struct EngineItf {
  SLresult (*CreateLEDDevice)(void *, void *, uint32_t, uint32_t, const void *, const void *);
  SLresult (*CreateVibraDevice)(void *, void *, uint32_t, uint32_t, const void *, const void *);
  SLresult (*CreateAudioPlayer)(void *self, void **pPlayer, void *src, void *sink, uint32_t n, const void **ids, const uint32_t *req);
  SLresult (*CreateAudioRecorder)(void *, void *, void *, void *, uint32_t, const void *, const void *);
  SLresult (*CreateMidiPlayer)(void *, void *, void *, void *, void *, void *, void *, uint32_t, const void *, const void *);
  SLresult (*CreateListener)(void *, void *, uint32_t, const void *, const void *);
  SLresult (*Create3DGroup)(void *, void *, uint32_t, const void *, const void *);
  SLresult (*CreateOutputMix)(void *self, void **pMix, uint32_t n, const void **ids, const uint32_t *req);
  SLresult (*CreateMetadataExtractor)(void *, void *, void *, uint32_t, const void *, const void *);
  SLresult (*CreateExtensionObject)(void *, void *, void *, uint32_t, uint32_t, const void *, const void *);
  SLresult (*QueryNumSupportedInterfaces)(void *, uint32_t, uint32_t *);
  SLresult (*QuerySupportedInterfaces)(void *, uint32_t, uint32_t, void *);
  SLresult (*QueryNumSupportedExtensions)(void *, uint32_t *);
  SLresult (*QuerySupportedExtension)(void *, uint32_t, void *, int16_t *);
  SLresult (*IsExtensionSupported)(void *, const void *, uint32_t *);
};

struct PlayItf {
  SLresult (*SetPlayState)(void *self, uint32_t state);
  SLresult (*GetPlayState)(void *self, uint32_t *state);
  SLresult (*GetDuration)(void *self, uint32_t *ms);
  SLresult (*GetPosition)(void *self, uint32_t *ms);
  SLresult (*RegisterCallback)(void *self, void *cb, void *ctx);
  SLresult (*SetCallbackEventsMask)(void *self, uint32_t mask);
  SLresult (*GetCallbackEventsMask)(void *self, uint32_t *mask);
  SLresult (*SetMarkerPosition)(void *self, uint32_t ms);
  SLresult (*ClearMarkerPosition)(void *self);
  SLresult (*GetMarkerPosition)(void *self, uint32_t *ms);
  SLresult (*SetPositionUpdatePeriod)(void *self, uint32_t ms);
  SLresult (*GetPositionUpdatePeriod)(void *self, uint32_t *ms);
};

typedef void (*BQCallback)(void *itf, void *ctx);

struct BufferQueueItf {
  SLresult (*Enqueue)(void *self, const void *buf, uint32_t size);
  SLresult (*Clear)(void *self);
  SLresult (*GetState)(void *self, uint32_t *state /* {count, index} */);
  SLresult (*RegisterCallback)(void *self, BQCallback cb, void *ctx);
};

struct VolumeItf {
  SLresult (*SetVolumeLevel)(void *self, int16_t mb);
  SLresult (*GetVolumeLevel)(void *self, int16_t *mb);
  SLresult (*GetMaxVolumeLevel)(void *self, int16_t *mb);
  SLresult (*SetMute)(void *self, uint32_t mute);
  SLresult (*GetMute)(void *self, uint32_t *mute);
  SLresult (*EnableStereoPosition)(void *self, uint32_t enable);
  SLresult (*IsEnabledStereoPosition)(void *self, uint32_t *enable);
  SLresult (*SetStereoPosition)(void *self, int16_t pos);
  SLresult (*GetStereoPosition)(void *self, int16_t *pos);
};

typedef struct { uint32_t locatorType, numBuffers; } LocatorBQ;
typedef struct { uint32_t formatType, numChannels, samplesPerSec, bitsPerSample, containerSize, channelMask, endianness; } FormatPCM;
typedef struct { void *pLocator; void *pFormat; } DataSource;

typedef struct {
  const struct ObjectItf *obj;
  const struct EngineItf *eng;
} Engine;

typedef struct {
  const struct ObjectItf *obj;
} OutputMix;

typedef struct {
  const struct ObjectItf *obj;
  const struct PlayItf *play;
  const struct BufferQueueItf *bq;
  const struct VolumeItf *vol;
  Mutex lock;
  uint32_t state;
  int channels, bits;
  int kind;                       /* 0 u8, 1 s16, 2 s32, 3 f32 */
  uint32_t step;                  /* 16.16 source frames per device frame */
  uint32_t frac;
  const void *qbuf[BQ_SLOTS];
  uint32_t qsize[BQ_SLOTS];
  int qhead, qcount;
  uint32_t cur_pos;               /* byte offset in the head buffer */
  BQCallback cb;
  void *cb_ctx;
  float gain;
  int muted;
  int16_t level_mb;
} Player;

static Player *g_players[MAX_PLAYERS];
static Mutex   g_players_lock;
static Thread  g_mix_thread;
static volatile int g_mix_stop;
static int     g_audio_up;

/* ---- object ---------------------------------------------------------------------- */

static SLresult obj_realize(void *self, uint32_t async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_resume(void *self, uint32_t async) { (void)self; (void)async; return SL_RESULT_SUCCESS; }
static SLresult obj_get_state(void *self, uint32_t *st) { (void)self; if (st) *st = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
static SLresult obj_register_cb(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static void     obj_abort(void *self) { (void)self; }
static SLresult obj_set_prio(void *self, int32_t p, uint32_t e) { (void)self; (void)p; (void)e; return SL_RESULT_SUCCESS; }
static SLresult obj_get_prio(void *self, int32_t *p, uint32_t *e) { (void)self; if (p) *p = 0; if (e) *e = 0; return SL_RESULT_SUCCESS; }
static SLresult obj_loss(void *self, int16_t n, void *i, uint32_t e) { (void)self; (void)n; (void)i; (void)e; return SL_RESULT_SUCCESS; }

/* engine object */
static SLresult engine_get_interface(void *self, const void *iid, void *out) {
  Engine *e = CONTAINER(self, Engine, obj);
  if (iid == SL_IID_ENGINE) { *(void **)out = &e->eng; return SL_RESULT_SUCCESS; }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void engine_destroy(void *self) { free(CONTAINER(self, Engine, obj)); }

/* output mix object */
static SLresult mix_get_interface(void *self, const void *iid, void *out) { (void)self; (void)iid; (void)out; return SL_RESULT_FEATURE_UNSUPPORTED; }
static void mix_destroy(void *self) { free(CONTAINER(self, OutputMix, obj)); }

/* player object */
static SLresult player_get_interface(void *self, const void *iid, void *out) {
  Player *p = CONTAINER(self, Player, obj);
  if (iid == SL_IID_PLAY) { *(void **)out = &p->play; return SL_RESULT_SUCCESS; }
  if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) { *(void **)out = &p->bq; return SL_RESULT_SUCCESS; }
  if (iid == SL_IID_VOLUME) { *(void **)out = &p->vol; return SL_RESULT_SUCCESS; }
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void player_destroy(void *self) {
  Player *p = CONTAINER(self, Player, obj);
  mutexLock(&g_players_lock);
  for (int i = 0; i < MAX_PLAYERS; i++) if (g_players[i] == p) g_players[i] = NULL;
  mutexUnlock(&g_players_lock);
  free(p);
}

static const struct ObjectItf k_engine_obj = { obj_realize, obj_resume, obj_get_state, engine_get_interface, obj_register_cb, obj_abort, engine_destroy, obj_set_prio, obj_get_prio, obj_loss };
static const struct ObjectItf k_mix_obj    = { obj_realize, obj_resume, obj_get_state, mix_get_interface, obj_register_cb, obj_abort, mix_destroy, obj_set_prio, obj_get_prio, obj_loss };
static const struct ObjectItf k_player_obj = { obj_realize, obj_resume, obj_get_state, player_get_interface, obj_register_cb, obj_abort, player_destroy, obj_set_prio, obj_get_prio, obj_loss };

/* ---- play / buffer queue / volume ------------------------------------------------------- */

static SLresult play_set_state(void *self, uint32_t st) {
  Player *p = CONTAINER(self, Player, play);
  mutexLock(&p->lock);
  p->state = st;
  if (st == SL_PLAYSTATE_STOPPED) { p->qhead = p->qcount = 0; p->cur_pos = 0; p->frac = 0; }
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult play_get_state(void *self, uint32_t *st) { *st = CONTAINER(self, Player, play)->state; return SL_RESULT_SUCCESS; }
static SLresult play_zero_u32(void *self, uint32_t *v) { (void)self; if (v) *v = 0; return SL_RESULT_SUCCESS; }
static SLresult play_register_cb(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
static SLresult play_set_u32(void *self, uint32_t v) { (void)self; (void)v; return SL_RESULT_SUCCESS; }
static SLresult play_clear_marker(void *self) { (void)self; return SL_RESULT_SUCCESS; }

static const struct PlayItf k_play = {
  play_set_state, play_get_state, play_zero_u32, play_zero_u32, play_register_cb, play_set_u32,
  play_zero_u32, play_set_u32, play_clear_marker, play_zero_u32, play_set_u32, play_zero_u32,
};

static SLresult bq_enqueue(void *self, const void *buf, uint32_t size) {
  Player *p = CONTAINER(self, Player, bq);
  if (!buf || !size) return SL_RESULT_PARAMETER_INVALID;
  mutexLock(&p->lock);
  if (p->qcount >= BQ_SLOTS) { mutexUnlock(&p->lock); return SL_RESULT_BUFFER_INSUFFICIENT; }
  int slot = (p->qhead + p->qcount) % BQ_SLOTS;
  p->qbuf[slot] = buf;
  p->qsize[slot] = size;
  if (p->qcount++ == 0) { p->cur_pos = 0; }
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_clear(void *self) {
  Player *p = CONTAINER(self, Player, bq);
  mutexLock(&p->lock);
  p->qhead = p->qcount = 0;
  p->cur_pos = 0;
  p->frac = 0;
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_get_state(void *self, uint32_t *st) {
  Player *p = CONTAINER(self, Player, bq);
  mutexLock(&p->lock);
  st[0] = (uint32_t)p->qcount;
  st[1] = (uint32_t)p->qhead;
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static SLresult bq_register_cb(void *self, BQCallback cb, void *ctx) {
  Player *p = CONTAINER(self, Player, bq);
  mutexLock(&p->lock);
  p->cb = cb;
  p->cb_ctx = ctx;
  mutexUnlock(&p->lock);
  return SL_RESULT_SUCCESS;
}
static const struct BufferQueueItf k_bq = { bq_enqueue, bq_clear, bq_get_state, bq_register_cb };

static void update_gain(Player *p) {
  p->gain = p->muted ? 0.0f : (p->level_mb <= -9600 ? 0.0f : (float)pow(10.0, p->level_mb / 2000.0));
}
static SLresult vol_set_level(void *self, int16_t mb) { Player *p = CONTAINER(self, Player, vol); p->level_mb = mb; update_gain(p); return SL_RESULT_SUCCESS; }
static SLresult vol_get_level(void *self, int16_t *mb) { *mb = CONTAINER(self, Player, vol)->level_mb; return SL_RESULT_SUCCESS; }
static SLresult vol_get_max(void *self, int16_t *mb) { (void)self; *mb = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_set_mute(void *self, uint32_t m) { Player *p = CONTAINER(self, Player, vol); p->muted = m != 0; update_gain(p); return SL_RESULT_SUCCESS; }
static SLresult vol_get_mute(void *self, uint32_t *m) { *m = (uint32_t)CONTAINER(self, Player, vol)->muted; return SL_RESULT_SUCCESS; }
static SLresult vol_enable_stereo(void *self, uint32_t e) { (void)self; (void)e; return SL_RESULT_SUCCESS; }
static SLresult vol_is_stereo(void *self, uint32_t *e) { (void)self; *e = 0; return SL_RESULT_SUCCESS; }
static SLresult vol_set_stereo(void *self, int16_t pos) { (void)self; (void)pos; return SL_RESULT_SUCCESS; }
static SLresult vol_get_stereo(void *self, int16_t *pos) { (void)self; *pos = 0; return SL_RESULT_SUCCESS; }
static const struct VolumeItf k_vol = { vol_set_level, vol_get_level, vol_get_max, vol_set_mute, vol_get_mute, vol_enable_stereo, vol_is_stereo, vol_set_stereo, vol_get_stereo };

/* ---- engine interface ------------------------------------------------------------------- */

static SLresult eng_unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static SLresult eng_create_output_mix(void *self, void **pMix, uint32_t n, const void **ids, const uint32_t *req) {
  (void)self; (void)n; (void)ids; (void)req;
  OutputMix *m = calloc(1, sizeof(*m));
  if (!m) return SL_RESULT_MEMORY_FAILURE;
  m->obj = &k_mix_obj;
  *pMix = &m->obj;
  return SL_RESULT_SUCCESS;
}

static SLresult eng_create_audio_player(void *self, void **pPlayer, void *srcv, void *sink, uint32_t n, const void **ids, const uint32_t *req) {
  (void)self; (void)sink;
  DataSource *src = srcv;
  if (!src || !src->pFormat) return SL_RESULT_PARAMETER_INVALID;
  for (uint32_t i = 0; i < n; i++) {
    int known = ids[i] == SL_IID_PLAY || ids[i] == SL_IID_BUFFERQUEUE || ids[i] == SL_IID_ANDROIDSIMPLEBUFFERQUEUE || ids[i] == SL_IID_VOLUME;
    if (!known && req && req[i]) { LOGE("OpenSL: required interface not supported"); return SL_RESULT_FEATURE_UNSUPPORTED; }
  }
  FormatPCM *fmt = src->pFormat;
  /* SL_DATAFORMAT_PCM (2) or SL_ANDROID_DATAFORMAT_PCM_EX (4), whose extra
   * 8th field is the representation: 1 signed int, 2 unsigned int, 3 float. */
  uint32_t repr = fmt->formatType == 4 ? ((const uint32_t *)src->pFormat)[7] : 0;
  int kind = -1;
  if (fmt->formatType == SL_DATAFORMAT_PCM || fmt->formatType == 4) {
    if (repr == 3 && fmt->bitsPerSample == 32) kind = 3;
    else if (repr != 3 && fmt->bitsPerSample == 32) kind = 2;
    else if (repr != 3 && fmt->bitsPerSample == 16) kind = 1;
    else if (repr != 3 && fmt->bitsPerSample == 8) kind = 0;
  }
  if (kind < 0 || fmt->numChannels < 1 || fmt->numChannels > 2) {
    LOGE("OpenSL: unsupported format type %u bits %u ch %u repr %u", fmt->formatType, fmt->bitsPerSample, fmt->numChannels, repr);
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  Player *p = calloc(1, sizeof(*p));
  if (!p) return SL_RESULT_MEMORY_FAILURE;
  p->obj = &k_player_obj;
  p->play = &k_play;
  p->bq = &k_bq;
  p->vol = &k_vol;
  mutexInit(&p->lock);
  p->state = SL_PLAYSTATE_STOPPED;
  p->channels = (int)fmt->numChannels;
  p->bits = (int)fmt->bitsPerSample;
  p->kind = kind;
  uint32_t rate = fmt->samplesPerSec / 1000;
  if (!rate) rate = DEV_RATE;
  p->step = (uint32_t)(((uint64_t)rate << 16) / DEV_RATE);
  p->gain = 1.0f;

  mutexLock(&g_players_lock);
  int slot = -1;
  for (int i = 0; i < MAX_PLAYERS; i++) if (!g_players[i]) { slot = i; break; }
  if (slot >= 0) g_players[slot] = p;
  mutexUnlock(&g_players_lock);
  if (slot < 0) { free(p); return SL_RESULT_MEMORY_FAILURE; }
  LOGI("OpenSL player: %u Hz, %d ch, %d bit", rate, p->channels, p->bits);
  *pPlayer = &p->obj;
  return SL_RESULT_SUCCESS;
}

static const struct EngineItf k_engine = {
  (void *)eng_unsupported, (void *)eng_unsupported, eng_create_audio_player, (void *)eng_unsupported,
  (void *)eng_unsupported, (void *)eng_unsupported, (void *)eng_unsupported, eng_create_output_mix,
  (void *)eng_unsupported, (void *)eng_unsupported, (void *)eng_unsupported, (void *)eng_unsupported,
  (void *)eng_unsupported, (void *)eng_unsupported, (void *)eng_unsupported,
};

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *options,
                        uint32_t numInterfaces, const void *interfaceIds, const void *required) {
  (void)numOptions; (void)options; (void)numInterfaces; (void)interfaceIds; (void)required;
  Engine *e = calloc(1, sizeof(*e));
  if (!e) return SL_RESULT_MEMORY_FAILURE;
  e->obj = &k_engine_obj;
  e->eng = &k_engine;
  *pEngine = &e->obj;
  return SL_RESULT_SUCCESS;
}

/* ---- mixing -------------------------------------------------------------------------------- */

static void mix_player(Player *p, int32_t *acc, int frames) {
  mutexLock(&p->lock);
  const int bpf = p->channels * (p->bits / 8);
  int i = 0;
  while (i < frames && p->state == SL_PLAYSTATE_PLAYING && p->qcount > 0) {
    const uint8_t *buf = p->qbuf[p->qhead];
    uint32_t size = p->qsize[p->qhead];
    if (p->cur_pos + (uint32_t)bpf > size) {
      p->qhead = (p->qhead + 1) % BQ_SLOTS;
      p->qcount--;
      p->cur_pos = 0;
      BQCallback cb = p->cb;
      void *ctx = p->cb_ctx;
      if (cb) {
        /* The game usually enqueues its next buffer from inside the callback. */
        mutexUnlock(&p->lock);
        cb(&p->bq, ctx);
        mutexLock(&p->lock);
      }
      continue;
    }
    int32_t l, r;
    const uint32_t bps = (uint32_t)(p->bits / 8);
    if (p->kind == 1) {
      int16_t s0, s1;
      memcpy(&s0, buf + p->cur_pos, 2);
      if (p->channels == 2) memcpy(&s1, buf + p->cur_pos + 2, 2); else s1 = s0;
      l = s0; r = s1;
    } else if (p->kind == 3) {
      float f0, f1;
      memcpy(&f0, buf + p->cur_pos, 4);
      if (p->channels == 2) memcpy(&f1, buf + p->cur_pos + bps, 4); else f1 = f0;
      f0 = f0 > 1.0f ? 1.0f : (f0 < -1.0f ? -1.0f : f0);
      f1 = f1 > 1.0f ? 1.0f : (f1 < -1.0f ? -1.0f : f1);
      l = (int32_t)(f0 * 32767.0f); r = (int32_t)(f1 * 32767.0f);
    } else if (p->kind == 2) {
      int32_t s0, s1;
      memcpy(&s0, buf + p->cur_pos, 4);
      if (p->channels == 2) memcpy(&s1, buf + p->cur_pos + bps, 4); else s1 = s0;
      l = s0 >> 16; r = s1 >> 16;
    } else {
      l = ((int32_t)buf[p->cur_pos] - 128) << 8;
      r = p->channels == 2 ? ((int32_t)buf[p->cur_pos + 1] - 128) << 8 : l;
    }
    acc[i * 2] += (int32_t)((float)l * p->gain);
    acc[i * 2 + 1] += (int32_t)((float)r * p->gain);
    i++;
    p->frac += p->step;
    p->cur_pos += (p->frac >> 16) * (uint32_t)bpf;
    p->frac &= 0xFFFF;
  }
  mutexUnlock(&p->lock);
}

static void mix_thread(void *arg) {
  (void)arg;
  bionic_tls_install(NULL);
  static AudioOutBuffer aob[DEV_BUFFERS];
  static int32_t acc[DEV_FRAMES * 2];
  const size_t data_size = DEV_FRAMES * 2 * sizeof(int16_t);
  for (int i = 0; i < DEV_BUFFERS; i++) {
    void *mem = memalign(0x1000, 0x1000);
    if (!mem) { LOGE("audio buffer allocation failed"); return; }
    memset(mem, 0, 0x1000);
    aob[i].next = NULL;
    aob[i].buffer = mem;
    aob[i].buffer_size = 0x1000;
    aob[i].data_size = data_size;
    aob[i].data_offset = 0;
    audoutAppendAudioOutBuffer(&aob[i]);
  }
  while (!g_mix_stop) {
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ull)) || !released) continue;
    memset(acc, 0, sizeof(acc));
    mutexLock(&g_players_lock);
    for (int i = 0; i < MAX_PLAYERS; i++) if (g_players[i]) mix_player(g_players[i], acc, DEV_FRAMES);
    mutexUnlock(&g_players_lock);
    int16_t *out = released->buffer;
    for (int i = 0; i < DEV_FRAMES * 2; i++) {
      int32_t v = acc[i];
      out[i] = (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
    released->data_size = data_size;
    audoutAppendAudioOutBuffer(released);
  }
}

int opensles_init(void) {
  if (g_audio_up) return 0;
  mutexInit(&g_players_lock);
  Result rc = audoutInitialize();
  if (R_FAILED(rc)) { LOGE("audoutInitialize failed: 0x%x (continuing silent)", rc); return -1; }
  audoutStartAudioOut();
  g_mix_stop = 0;
  rc = threadCreate(&g_mix_thread, mix_thread, NULL, NULL, 0x20000, 0x2B, 2);
  if (R_FAILED(rc)) rc = threadCreate(&g_mix_thread, mix_thread, NULL, NULL, 0x20000, 0x2B, -2);  /* core 2 not allowed (applet mode) */
  if (R_FAILED(rc)) { LOGE("audio thread create failed: 0x%x", rc); audoutExit(); return -1; }
  threadStart(&g_mix_thread);
  g_audio_up = 1;
  LOGI("audio: %d Hz stereo, %d-frame buffers", DEV_RATE, DEV_FRAMES);
  return 0;
}

void opensles_shutdown(void) {
  if (!g_audio_up) return;
  g_mix_stop = 1;
  threadWaitForExit(&g_mix_thread);
  threadClose(&g_mix_thread);
  audoutStopAudioOut();
  audoutExit();
  g_audio_up = 0;
}
