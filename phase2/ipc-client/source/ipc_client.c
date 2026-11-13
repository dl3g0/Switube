#include "switube/ipc_client.h"

#include <string.h>

static Service g_switubeService;

Result switubeIpcInitialize(void) {
  if (serviceIsActive(&g_switubeService)) return 0;
  return smGetService(&g_switubeService, SWITUBE_IPC_SERVICE_NAME);
}

void switubeIpcExit(void) { serviceClose(&g_switubeService); }

bool switubeIpcIsActive(void) { return serviceIsActive(&g_switubeService); }

Result switubeIpcPing(void) {
  return serviceDispatch(&g_switubeService, SwitubeIpcCommand_Ping);
}

Result switubeIpcGetApiVersion(uint32_t* version) {
  if (!version) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  return serviceDispatchOut(&g_switubeService,
                            SwitubeIpcCommand_GetApiVersion, *version);
}

Result switubeIpcGetCapabilities(uint64_t* capabilities) {
  if (!capabilities) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  return serviceDispatchOut(&g_switubeService,
                            SwitubeIpcCommand_GetCapabilities, *capabilities);
}

Result switubeIpcGetStatus(SwitubeIpcStatus* status) {
  if (!status) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  memset(status, 0, sizeof(*status));
  return serviceDispatchOut(&g_switubeService, SwitubeIpcCommand_GetStatus,
                            *status);
}

static Result dispatchNoInput(SwitubeIpcCommand command) {
  return serviceDispatch(&g_switubeService, command);
}

static Result dispatchU32(SwitubeIpcCommand command, uint32_t value) {
  return serviceDispatchIn(&g_switubeService, command, value);
}

Result switubeIpcPlayPause(void) {
  return dispatchNoInput(SwitubeIpcCommand_PlayPause);
}

Result switubeIpcPlay(void) {
  return dispatchNoInput(SwitubeIpcCommand_Play);
}

Result switubeIpcPause(void) {
  return dispatchNoInput(SwitubeIpcCommand_Pause);
}

Result switubeIpcNext(void) {
  return dispatchNoInput(SwitubeIpcCommand_Next);
}

Result switubeIpcPrevious(void) {
  return dispatchNoInput(SwitubeIpcCommand_Previous);
}

Result switubeIpcSetVolume(uint32_t volumePermille) {
  return dispatchU32(SwitubeIpcCommand_SetVolume, volumePermille);
}

Result switubeIpcSeek(uint32_t positionMs) {
  return dispatchU32(SwitubeIpcCommand_Seek, positionMs);
}

Result switubeIpcSetShuffle(bool enabled) {
  return dispatchU32(SwitubeIpcCommand_SetShuffle, enabled ? 1u : 0u);
}

Result switubeIpcSetRepeatMode(SwitubeRepeatMode mode) {
  return dispatchU32(SwitubeIpcCommand_SetRepeatMode, (uint32_t)mode);
}

Result switubeIpcSetAudioQuality(bool highQuality) {
  return dispatchU32(SwitubeIpcCommand_SetAudioQuality,
                     highQuality ? 1u : 0u);
}

Result switubeIpcGetBrowseHeader(SwitubeIpcBrowseHeader* header) {
  if (!header) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  memset(header, 0, sizeof(*header));
  return serviceDispatchOut(&g_switubeService,
                            SwitubeIpcCommand_GetBrowseHeader, *header);
}

Result switubeIpcGetBrowseItem(uint32_t index, SwitubeIpcBrowseItem* item) {
  if (!item) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
  memset(item, 0, sizeof(*item));
  return serviceDispatchInOut(&g_switubeService,
                              SwitubeIpcCommand_GetBrowseItem, index, *item);
}

static Result dispatchBrowse(SwitubeIpcCommand command,
                             const SwitubeIpcBrowseRequest* request) {
  return serviceDispatchIn(&g_switubeService, command, *request);
}

Result switubeIpcBrowseHome(void) {
  SwitubeIpcBrowseRequest request = {0};
  return dispatchBrowse(SwitubeIpcCommand_BrowseHome, &request);
}

Result switubeIpcBrowseLibrary(void) {
  SwitubeIpcBrowseRequest request = {0};
  return dispatchBrowse(SwitubeIpcCommand_BrowseLibrary, &request);
}

Result switubeIpcBrowseSearch(const char* query) {
  SwitubeIpcBrowseRequest request = {0};
  if (query) {
    strncpy(request.query, query, sizeof(request.query) - 1);
    request.query[sizeof(request.query) - 1] = '\0';
  }
  return dispatchBrowse(SwitubeIpcCommand_BrowseSearch, &request);
}

Result switubeIpcBrowseOpen(uint64_t generation, uint32_t index) {
  SwitubeIpcBrowseRequest request = {0};
  request.generation = generation;
  request.index = index;
  return dispatchBrowse(SwitubeIpcCommand_BrowseOpen, &request);
}

Result switubeIpcBrowseBack(uint64_t generation) {
  SwitubeIpcBrowseRequest request = {0};
  request.generation = generation;
  return dispatchBrowse(SwitubeIpcCommand_BrowseBack, &request);
}

Result switubeIpcBrowsePlay(uint64_t generation, uint32_t index) {
  SwitubeIpcBrowseRequest request = {0};
  request.generation = generation;
  request.index = index;
  return dispatchBrowse(SwitubeIpcCommand_BrowsePlay, &request);
}

Result switubeIpcBrowseCurrentPlaylist(void) {
  SwitubeIpcBrowseRequest request = {0};
  return dispatchBrowse(SwitubeIpcCommand_BrowseCurrentPlaylist, &request);
}

Result switubeIpcBrowseQueue(void) {
  SwitubeIpcBrowseRequest request = {0};
  return dispatchBrowse(SwitubeIpcCommand_BrowseQueue, &request);
}

