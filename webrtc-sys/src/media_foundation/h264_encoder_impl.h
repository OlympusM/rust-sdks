#ifndef WEBRTC_MEDIA_FOUNDATION_H264_ENCODER_IMPL_H_
#define WEBRTC_MEDIA_FOUNDATION_H264_ENCODER_IMPL_H_

#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wrl/client.h>

#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "api/transport/rtp/dependency_descriptor.h"
#include "api/video/color_space.h"
#include "api/video/encoded_image.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_codec_constants.h"
#include "api/video/video_frame.h"
#include "api/video/video_frame_type.h"
#include "api/video_codecs/scalability_mode.h"
#include "api/video_codecs/video_encoder.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "modules/video_coding/codecs/h264/include/h264.h"

namespace webrtc {

class MediaFoundationH264EncoderImpl : public VideoEncoder {
 public:
  MediaFoundationH264EncoderImpl(const webrtc::Environment& env,
                                 const SdpVideoFormat& format);
  ~MediaFoundationH264EncoderImpl() override;

  int32_t InitEncode(const VideoCodec* codec_settings,
                     const Settings& settings) override;

  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override;

  int32_t Release() override;

  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override;

  void SetRates(const RateControlParameters& rc_parameters) override;

  EncoderInfo GetEncoderInfo() const override;

 private:
  // Enumerates and activates a hardware H264 encoder MFT via MFTEnumEx.
  // Returns false if none is available. Also acquires the process-wide MF
  // runtime (MFStartup) and this thread's COM apartment on first success.
  bool CreateEncoderMFT();

  // Configures NV12 input / H264 output media types on transform_ and
  // queries the resulting output stream requirements. Requires transform_
  // to already be created.
  bool ConfigureMediaTypes(int width, int height, int fps, uint32_t bitrate_bps);

  // Applies ICodecAPI settings: rate control mode, bitrate, low latency,
  // CABAC. Non-fatal if individual properties are unsupported by the
  // driver's MFT.
  bool ConfigureCodecApi(uint32_t bitrate_bps, int fps);

  // Converts `frame` to NV12, wraps it in an IMFSample and submits it via
  // ProcessInput. Returns the raw HRESULT so the caller can special-case
  // MF_E_NOTACCEPTING (drain output, then retry). Used on the synchronous
  // path only (is_async_ == false).
  HRESULT SubmitInput(const VideoFrame& frame, bool force_keyframe);

  // Drains all currently available output samples from transform_,
  // delivering each to encoded_image_callback_. Returns false on an
  // unrecoverable error. Used on the synchronous path only.
  bool DrainOutput(const VideoFrame& source_frame);

  // Handles MF_E_TRANSFORM_STREAM_CHANGE by re-querying and re-setting the
  // output media type and stream info.
  bool HandleStreamChange();

  // Flushes and tears down transform_/codec_api_. Safe to call multiple
  // times and when never successfully created.
  void DestroyEncoder();

  // --- Async MFT support ---
  //
  // Most hardware encoder MFTs (including NVIDIA's and Intel Quick Sync's)
  // are asynchronous transforms. Per Microsoft's documented requirement,
  // calling SetInputType/SetOutputType/ProcessInput/ProcessOutput on one
  // before completing the async "unlock" handshake fails with
  // MF_E_TRANSFORM_ASYNC_LOCKED. Once unlocked, ProcessInput/ProcessOutput
  // must only be called in direct response to METransformNeedInput /
  // METransformHaveOutput events from the MFT's IMFMediaEventGenerator --
  // never called freely the way the synchronous model allows.

  // Queries the MF_TRANSFORM_ASYNC attribute and, if set, performs the
  // unlock handshake (MF_TRANSFORM_ASYNC_UNLOCK) and obtains
  // event_generator_. Sets is_async_ accordingly. Always returns true
  // unless a genuine COM error occurs querying attributes -- a transform
  // that simply isn't async is not a failure case here.
  bool UnlockAsyncTransformIfNeeded();

  // Converts `frame` to NV12 and submits it through the async event-driven
  // protocol: blocks until the MFT has signaled it's ready for input
  // (consuming a queued METransformNeedInput credit), then calls
  // ProcessInput. Any METransformHaveOutput events encountered while
  // waiting are delivered along the way. Returns false on an unrecoverable
  // error.
  bool SubmitInputAsync(const VideoFrame& frame, bool force_keyframe);

  // Non-blocking drain of any currently queued MFT events -- used right
  // after ProcessInput to opportunistically pick up output without forcing
  // the caller to block past its frame cadence. Same event handling as the
  // blocking path.
  bool PumpAvailableEventsNonBlocking();

  // Handles one MFT event: for METransformHaveOutput, pulls and delivers
  // the encoded sample via DeliverNextOutputSample(). For
  // METransformNeedInput, increments async_need_input_count_. Returns
  // false on an unrecoverable error.
  bool HandleAsyncEvent(IMFMediaEvent* event);

  // Pulls one output sample via ProcessOutput and delivers it through
  // encoded_image_callback_, using the oldest entry in pending_frames_ for
  // timestamp/color-space metadata. H264 encode preserves input order at
  // the output (no reordering), so FIFO pairing is correct.
  bool DeliverNextOutputSample();

  struct PendingFrameInfo {
    uint32_t rtp_timestamp = 0;
    int64_t capture_time_ms = 0;
    std::optional<ColorSpace> color_space;
  };
  std::deque<PendingFrameInfo> pending_frames_;

  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator_;
  bool is_async_ = false;
  // Number of METransformNeedInput events received but not yet consumed by
  // a successful ProcessInput call. The MFT may signal readiness for
  // several inputs in a row before we catch up, so this is a counter, not
  // a bool.
  int async_need_input_count_ = 0;

  const webrtc::Environment& env_;
  EncodedImageCallback* encoded_image_callback_ = nullptr;

  VideoCodec codec_;
  EncodedImage encoded_image_;
  H264PacketizationMode packetization_mode_;
  const SdpVideoFormat format_;
  H264Profile profile_ = H264Profile::kProfileConstrainedBaseline;

  webrtc::H264BitstreamParser h264_bitstream_parser_;

  // Media Foundation state.
  Microsoft::WRL::ComPtr<IMFTransform> transform_;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api_;
  bool com_initialized_ = false;    // We own CoUninitialize().
  bool mf_runtime_acquired_ = false;  // We own one MFStartup/MFShutdown pair.
  bool streaming_started_ = false;
  bool output_provides_samples_ = false;
  DWORD output_sample_min_size_ = 0;
  DWORD output_sample_alignment_ = 0;

  // Scratch buffer reused across frames for the I420->NV12 conversion.
  std::vector<uint8_t> nv12_buffer_;

  int64_t frame_count_ = 0;
};

}  // namespace webrtc

#endif  // WEBRTC_MEDIA_FOUNDATION_H264_ENCODER_IMPL_H_