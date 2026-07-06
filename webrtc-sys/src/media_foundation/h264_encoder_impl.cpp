#include "h264_encoder_impl.h"

#include <algorithm>
#include <limits>
#include <string>

#include "absl/strings/match.h"
#include "absl/types/optional.h"
#include "api/video/video_codec_constants.h"
#include "common_video/h264/h264_common.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace webrtc {

MediaFoundationH264EncoderImpl::MediaFoundationH264EncoderImpl(
    const webrtc::Environment& env,
    const SdpVideoFormat& format)
    : env_(env),
      packetization_mode_(
          H264EncoderSettings::Parse(format).packetization_mode),
      format_(format) {
  std::string hexString = format_.parameters.at("profile-level-id");
  std::optional<webrtc::H264ProfileLevelId> profile_level_id =
      webrtc::ParseH264ProfileLevelId(hexString.c_str());
  if (profile_level_id.has_value()) {
    profile_ = profile_level_id->profile;
  }
}

MediaFoundationH264EncoderImpl::~MediaFoundationH264EncoderImpl() {
  Release();
}

int32_t MediaFoundationH264EncoderImpl::InitEncode(
    const VideoCodec* inst,
    const VideoEncoder::Settings& settings) {
  if (!inst || inst->codecType != kVideoCodecH264) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->maxFramerate == 0) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->width < 1 || inst->height < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

  codec_ = *inst;

  // Initialize encoded image
  const size_t new_capacity =
      CalcBufferSize(VideoType::kI420, codec_.width, codec_.height);
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
  encoded_image_._encodedWidth = codec_.width;
  encoded_image_._encodedHeight = codec_.height;
  encoded_image_.set_size(0);

  RTC_LOG(LS_INFO) << "Media Foundation H264 encoder initialized: "
                   << codec_.width << "x" << codec_.height
                   << " @ " << codec_.maxFramerate << "fps";

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MediaFoundationH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MediaFoundationH264EncoderImpl::Release() {
  // TODO: Release Media Foundation encoder resources
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MediaFoundationH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!encoded_image_callback_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  // TODO: Implement actual Media Foundation encoding
  RTC_LOG(LS_WARNING) << "Media Foundation H264 Encode not yet implemented";
  
  return WEBRTC_VIDEO_CODEC_OK;
}

void MediaFoundationH264EncoderImpl::SetRates(
    const RateControlParameters& parameters) {
  if (parameters.framerate_fps < 1.0) {
    RTC_LOG(LS_WARNING) << "Invalid frame rate: " << parameters.framerate_fps;
    return;
  }
  
  codec_.maxFramerate = static_cast<uint32_t>(parameters.framerate_fps);
  codec_.maxBitrate = parameters.bitrate.GetSpatialLayerSum(0);
  
  RTC_LOG(LS_INFO) << "Media Foundation H264 SetRates: "
                   << parameters.framerate_fps << "fps, "
                   << parameters.bitrate.GetSpatialLayerSum(0) << " bps";
}

VideoEncoder::EncoderInfo MediaFoundationH264EncoderImpl::GetEncoderInfo() const {
  EncoderInfo info;
  info.supports_native_handle = false;
  info.implementation_name = "Media Foundation H264 Encoder";
  info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
  info.is_hardware_accelerated = true;
  info.supports_simulcast = false;
  info.preferred_pixel_formats = {VideoFrameBuffer::Type::kI420};
  return info;
}

}  // namespace webrtc