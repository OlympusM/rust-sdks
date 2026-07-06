#include "media_foundation_encoder_factory.h"

#include <memory>

#include "h264_encoder_impl.h"
#include "rtc_base/logging.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>

namespace webrtc {

MediaFoundationVideoEncoderFactory::MediaFoundationVideoEncoderFactory() {
  std::map<std::string, std::string> baselineParameters = {
      {"profile-level-id", "42e01f"},
      {"level-asymmetry-allowed", "1"},
      {"packetization-mode", "1"},
  };
  supported_formats_.push_back(SdpVideoFormat("H264", baselineParameters));
}

MediaFoundationVideoEncoderFactory::~MediaFoundationVideoEncoderFactory() {}

bool MediaFoundationVideoEncoderFactory::IsSupported() {
  // TODO: Probe for Media Foundation H.264 encoder availability
  // For now, return true to indicate Media Foundation is supported on Windows
  RTC_LOG(LS_INFO) << "Media Foundation encoder support check (stub)";
  return true;
}

std::unique_ptr<VideoEncoder> MediaFoundationVideoEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  // Check if the requested format is supported.
  for (const auto& supported_format : supported_formats_) {
    if (format.IsSameCodec(supported_format)) {
      if (format.name == "H264") {
        RTC_LOG(LS_INFO) << "Using Media Foundation HW encoder for H264";
        return std::make_unique<MediaFoundationH264EncoderImpl>(env, format);
      }
    }
  }
  return nullptr;
}

std::vector<SdpVideoFormat> MediaFoundationVideoEncoderFactory::GetSupportedFormats()
    const {
  return supported_formats_;
}

std::vector<SdpVideoFormat> MediaFoundationVideoEncoderFactory::GetImplementations()
    const {
  return supported_formats_;
}

}  // namespace webrtc