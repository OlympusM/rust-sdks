#ifndef WEBRTC_MEDIA_FOUNDATION_H264_ENCODER_IMPL_H_
#define WEBRTC_MEDIA_FOUNDATION_H264_ENCODER_IMPL_H_

#include <memory>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "api/transport/rtp/dependency_descriptor.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_codec_constants.h"
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
  const webrtc::Environment& env_;
  EncodedImageCallback* encoded_image_callback_ = nullptr;
  
  VideoCodec codec_;
  EncodedImage encoded_image_;
  H264PacketizationMode packetization_mode_;
  const SdpVideoFormat format_;
  H264Profile profile_ = H264Profile::kProfileConstrainedBaseline;
  
  webrtc::H264BitstreamParser h264_bitstream_parser_;
};

}  // namespace webrtc

#endif  // WEBRTC_MEDIA_FOUNDATION_H264_ENCODER_IMPL_H_