#pragma once

#include "switube/ipc_protocol.h"

#include <switch.h>

#ifdef __cplusplus
extern "C" {
#endif

Result switubeIpcInitialize(void);
void switubeIpcExit(void);
bool switubeIpcIsActive(void);

Result switubeIpcPing(void);
Result switubeIpcGetApiVersion(uint32_t* version);
Result switubeIpcGetCapabilities(uint64_t* capabilities);
Result switubeIpcGetStatus(SwitubeIpcStatus* status);

Result switubeIpcPlayPause(void);
Result switubeIpcPlay(void);
Result switubeIpcPause(void);
Result switubeIpcNext(void);
Result switubeIpcPrevious(void);
Result switubeIpcSetVolume(uint32_t volumePermille);
Result switubeIpcSeek(uint32_t positionMs);
Result switubeIpcSetShuffle(bool enabled);
Result switubeIpcSetRepeatMode(SwitubeRepeatMode mode);
Result switubeIpcSetAudioQuality(bool highQuality);

Result switubeIpcGetBrowseHeader(SwitubeIpcBrowseHeader* header);
Result switubeIpcGetBrowseItem(uint32_t index, SwitubeIpcBrowseItem* item);
Result switubeIpcBrowseHome(void);
Result switubeIpcBrowseLibrary(void);
Result switubeIpcBrowseSearch(const char* query);
Result switubeIpcBrowseOpen(uint64_t generation, uint32_t index);
Result switubeIpcBrowseBack(uint64_t generation);
Result switubeIpcBrowsePlay(uint64_t generation, uint32_t index);
Result switubeIpcBrowseCurrentPlaylist(void);
Result switubeIpcBrowseQueue(void);

#ifdef __cplusplus
}
#endif
