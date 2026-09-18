#ifndef BK_OPENSLES_H
#define BK_OPENSLES_H

#include <stdint.h>

/* OpenSL ES 1.0.1 subset over libnx audout (48 kHz stereo s16): Engine,
 * OutputMix, and buffer-queue AudioPlayers with Play/BufferQueue/Volume. That
 * is everything Defold's sound device uses. Structure follows the audout
 * OpenSL shim in pvzultimate_nx; the mixing thread here also installs a bionic
 * TLS block, because buffer-queue callbacks run game code. */

int  opensles_init(void);
void opensles_shutdown(void);

extern void *SL_IID_NULL, *SL_IID_OBJECT, *SL_IID_ENGINE, *SL_IID_OUTPUTMIX, *SL_IID_PLAY,
            *SL_IID_BUFFERQUEUE, *SL_IID_ANDROIDSIMPLEBUFFERQUEUE, *SL_IID_VOLUME,
            *SL_IID_PLAYBACKRATE, *SL_IID_SEEK, *SL_IID_EFFECTSEND, *SL_IID_PREFETCHSTATUS,
            *SL_IID_ANDROIDCONFIGURATION, *SL_IID_METADATAEXTRACTION, *SL_IID_ENVIRONMENTALREVERB;

uint32_t slCreateEngine(void **pEngine, uint32_t numOptions, const void *options,
                        uint32_t numInterfaces, const void *interfaceIds, const void *required);

#endif
