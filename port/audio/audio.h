/* Audio of the wasm port (session S7): aspMain HLE (audio_hle.c) and the virtual AI (audio_ai.c).
 * Design and sources: docs/port/AUDIO.md. Written for this port from the ABI (ultralib include/PR/abi.h) and the way
 * the game's libaudio (ultralib src/audio, libaudio, not n_audio) builds its command lists; no HLE code of other
 * projects was used. */
#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H
#include <stdint.h>

/* run one M_AUDTASK (OSTask_t in game memory): the command list at data_ptr, data_size bytes */
void port_audio_task(void *task);

/* virtual AI (audio interface): osAiSetFrequency / osAiSetNextBuffer / AI registers, consumed per retrace */
int port_ai_set_frequency(int hz);           /* returns the DAC's real rate, as libultra */
int port_ai_set_next_buffer(void *buf, uint32_t size);
uint32_t port_ai_get_length(void);           /* AI_LEN_REG: bytes left in the current DMA */
uint32_t port_ai_get_status(void);           /* AI_STATUS_REG: FIFO full / busy bits */
void port_ai_retrace(void);                  /* one retrace of DAC time (port_vi_retrace) */
#endif
