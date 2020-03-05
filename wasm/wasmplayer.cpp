#include "moonlight_wasm.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>

#include <h264_stream.h>

#include <pthread.h>

#include "html/elementary_media_stream_source/elementary_audio_stream_track_config.h"
#include "html/elementary_media_stream_source/elementary_media_packet.h"
#include "html/elementary_media_stream_source/elementary_video_stream_track_config.h"
#include "html/html_media_element_listener.h"

#define INITIAL_DECODE_BUFFER_LEN 128 * 1024

using std::chrono_literals::operator""s;
using std::chrono_literals::operator""ms;
using EmssReadyState = Samsung::HTML::ElementaryMediaStreamSource::ReadyState;
using TimeStamp = std::chrono::duration<double>;

static constexpr TimeStamp kFrameTimeMargin = 0.5ms;
static constexpr TimeStamp kTimeWindow = 1s;

static bool s_FramePacingEnabled = false;

static uint32_t s_Width = 0;
static uint32_t s_Height = 0;
static uint32_t s_Framerate = 0;

static std::vector<unsigned char> s_DecodeBuffer;

static TimeStamp s_frameDuration;
static TimeStamp s_pktPts;

static TimeStamp s_ptsDiff;
static TimeStamp s_lastSec;

static std::chrono::time_point<std::chrono::steady_clock> s_firstAppend;
static std::chrono::time_point<std::chrono::steady_clock> s_lastTime;
static bool s_hasFirstFrame = false;

void MoonlightInstance::DidChangeFocus(bool got_focus) {
  // Request an IDR frame to dump the frame queue that may have
  // built up from the GL pipeline being stalled.
  if (got_focus) {
    g_Instance->m_RequestIdrFrame = true;
  }
}

bool MoonlightInstance::InitializeRenderingSurface(int width, int height) {
  return true;
}

int MoonlightInstance::StartupVidDecSetup(int videoFormat, int width,
                                          int height, int redrawRate,
                                          void* context, int drFlags) {
  g_Instance->m_MediaElement.SetSrc(g_Instance->m_Source.CreateObjectURL());
  ClLogMessage("Waiting for closed\n");
  g_Instance->waitFor(&g_Instance->m_EmssStateChanged, [] {
      return g_Instance->m_EmssReadyState == EmssReadyState::kClosed;
  });
  ClLogMessage("closed done\n");

  g_Instance->m_AudioTrack = g_Instance->m_Source.AddTrack(
      Samsung::HTML::ElementaryAudioStreamTrackConfig {
          "audio/webm; codecs=\"pcm\"",  // mimeType
          {},  // extradata (empty?)
          Samsung::HTML::SampleFormat::SampleFormatS16,
          Samsung::HTML::ChannelLayout::ChannelLayoutStereo,
          48000
  });

  g_Instance->m_AudioTrack.SetListener(&g_Instance->m_AudioTrackListener);

  g_Instance->m_VideoTrack = g_Instance->m_Source.AddTrack(
      Samsung::HTML::ElementaryVideoStreamTrackConfig{
          "video/mp4; codecs=\"hev1.1.6.L93.B0\"",  // h265 mimeType
          {},                                   // extradata (empty?)
          static_cast<uint32_t>(width),
          static_cast<uint32_t>(height),
          static_cast<uint32_t>(redrawRate),  // framerateNum
          1,                                  // framerateDen
      });

  g_Instance->m_VideoTrack.SetListener(&g_Instance->m_VideoTrackListener);

  ClLogMessage("Inb4 source open\n");
  g_Instance->m_Source.Open([](const char*) {});
  g_Instance->waitFor(&g_Instance->m_EmssStateChanged, [] {
      return g_Instance->m_EmssReadyState == EmssReadyState::kOpenPending;
  });
  ClLogMessage("Source ready to open\n");
  g_Instance->m_MediaElement.Play([](const char* err) {
    if (err) {
      ClLogMessage("Play error: %s\n", err);
    } else {
      ClLogMessage("Play success\n");
    }
  });

  ClLogMessage("Waiting for start\n");
  g_Instance->waitFor(&g_Instance->m_EmssAudioStateChanged,
                      [] { return g_Instance->m_AudioStarted.load(); });

  g_Instance->waitFor(&g_Instance->m_EmssVideoStateChanged,
                      [] { return g_Instance->m_VideoStarted.load(); });
  ClLogMessage("started\n");
  return 0;
}

int MoonlightInstance::VidDecSetup(int videoFormat, int width, int height,
                                   int redrawRate, void* context, int drFlags) {
  ClLogMessage("MoonlightInstance::VidDecSetup\n");
  s_DecodeBuffer.resize(INITIAL_DECODE_BUFFER_LEN);

  s_Width = width;
  s_Height = height;
  s_Framerate = redrawRate;

  s_frameDuration = TimeStamp(1.0 / redrawRate);
  s_pktPts = 0s;
  s_hasFirstFrame = false;
  s_lastSec = 0s;
  s_ptsDiff = 0s;

  s_FramePacingEnabled = g_Instance->m_FramePacingEnabled;

  static std::once_flag once_flag;
  std::call_once(once_flag, &MoonlightInstance::StartupVidDecSetup,
                 videoFormat, width, height, redrawRate, context, drFlags);
  return DR_OK;
}

void MoonlightInstance::VidDecCleanup(void) {
  s_DecodeBuffer.clear();
  s_DecodeBuffer.shrink_to_fit();
}

int MoonlightInstance::VidDecSubmitDecodeUnit(PDECODE_UNIT decodeUnit) {
  // ClLogMessage("MoonlightInstance::VidDecSubmitDecodeUnit\n");

  if (!g_Instance->m_VideoStarted)
    return DR_OK;

  PLENTRY entry;
  unsigned int offset;
  unsigned int totalLength;
  // ClLogMessage("Video packet append at: %f\n", s_pktPts);

  // Request an IDR frame if needed
  if (g_Instance->m_RequestIdrFrame) {
    g_Instance->m_RequestIdrFrame = false;
    return DR_NEED_IDR;
  }

  totalLength = decodeUnit->fullLength;
  // Resize the decode buffer if needed
  if (totalLength > s_DecodeBuffer.size()) {
    s_DecodeBuffer.resize(totalLength);
  }

  entry = decodeUnit->bufferList;
  offset = 0;
  while (entry != NULL) {
    memcpy(&s_DecodeBuffer[offset], entry->data, entry->length);
    offset += entry->length;
    entry = entry->next;
  }

  auto now = std::chrono::steady_clock::now();
  if (!s_hasFirstFrame) {
    s_firstAppend = std::chrono::steady_clock::now();
    s_hasFirstFrame = true;
  } else if (s_FramePacingEnabled) {
    TimeStamp fromStart = now - s_firstAppend;

    while (s_pktPts > fromStart - s_ptsDiff + kFrameTimeMargin) {
      now = std::chrono::steady_clock::now();
      fromStart = now - s_firstAppend;
    }

    if (fromStart > s_lastSec + kTimeWindow) {
      s_lastSec += kTimeWindow;
      s_ptsDiff = fromStart - s_pktPts;
    }
  }
  s_lastTime = now;

  // Start the decoding
  uint32_t packedMillis = ProfilerGetPackedMillis();
  Samsung::HTML::ElementaryMediaPacket pkt{
      s_pktPts.count(),        s_pktPts.count(),
      s_frameDuration.count(), decodeUnit->frameType == FRAME_TYPE_IDR,
      offset,          s_DecodeBuffer.data(),
      s_Width,         s_Height,
      s_Framerate,     1};
  g_Instance->m_VideoTrack.AppendPacket(pkt);

  s_pktPts += s_frameDuration;

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS MoonlightInstance::s_DrCallbacks = {
    .setup = MoonlightInstance::VidDecSetup,
    .cleanup = MoonlightInstance::VidDecCleanup,
    .submitDecodeUnit = MoonlightInstance::VidDecSubmitDecodeUnit,
    .capabilities = CAPABILITY_SLICES_PER_FRAME(4)};
