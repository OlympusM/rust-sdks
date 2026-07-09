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

class Environment;

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
  bool CreateEncoderMFT();
  bool ConfigureMediaTypes(int width, int height, int fps, uint32_t bitrate_bps);
  bool ConfigureCodecApi(uint32_t bitrate_bps, int fps);
  HRESULT SubmitInput(const VideoFrame& frame, bool force_keyframe);

  bool DrainOutput(const VideoFrame& source_frame);
  bool HandleStreamChange();
  void DestroyEncoder();

  bool UnlockAsyncTransformIfNeeded();
  bool SubmitInputAsync(const VideoFrame& frame, bool force_keyframe);
  bool PumpAvailableEventsNonBlocking();
  bool HandleAsyncEvent(IMFMediaEvent* event);
  bool DeliverNextOutputSample();

  struct PendingFrameInfo {
    uint32_t rtp_timestamp = 0;
    int64_t capture_time_ms = 0;
    std::optional<ColorSpace> color_space;
  };
  std::deque<PendingFrameInfo> pending_frames_;

  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> event_generator_;
  bool is_async_ = false;
  int async_need_input_count_ = 0;

  const webrtc::Environment& env_;
  EncodedImageCallback* encoded_image_callback_ = nullptr;

  VideoCodec codec_;
  EncodedImage encoded_image_;
  H264PacketizationMode packetization_mode_;
  const SdpVideoFormat format_;
  H264Profile profile_ = H264Profile::kProfileConstrainedBaseline;

  webrtc::H264BitstreamParser h264_bitstream_parser_;

  Microsoft::WRL::ComPtr<IMFTransform> transform_;
  Microsoft::WRL::ComPtr<ICodecAPI> codec_api_;
  bool com_initialized_ = false;
  bool mf_runtime_acquired_ = false;
  bool streaming_started_ = false;
  bool output_provides_samples_ = false;
  DWORD output_sample_min_size_ = 0;
  DWORD output_sample_alignment_ = 0;

  std::vector<uint8_t> nv12_buffer_;

  int64_t frame_count_ = 0;
};

}

#endif