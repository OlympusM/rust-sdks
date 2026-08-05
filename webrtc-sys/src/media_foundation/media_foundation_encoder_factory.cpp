#include "media_foundation_encoder_factory.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <windows.h>

#include <cstdio>
#include <memory>

#include "h264_encoder_impl.h"
#include "rtc_base/logging.h"

namespace webrtc {

namespace {

bool ProbeHardwareH264Encoder() {
  fprintf(stderr, "[MF-DIAG] ProbeHardwareH264Encoder() starting\n");
  fflush(stderr);

  const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool co_owned = SUCCEEDED(co_hr);
  fprintf(stderr, "[MF-DIAG] CoInitializeEx hr=0x%08lX co_owned=%d\n",
          static_cast<unsigned long>(co_hr), co_owned);
  fflush(stderr);

  const HRESULT mf_hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  fprintf(stderr, "[MF-DIAG] MFStartup hr=0x%08lX\n",
          static_cast<unsigned long>(mf_hr));
  fflush(stderr);

  bool supported = false;

  if (SUCCEEDED(mf_hr)) {
    MFT_REGISTER_TYPE_INFO output_type_info = {MFMediaType_Video,
                                               MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;

    const HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
            MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &output_type_info, &activates, &count);

    fprintf(stderr,
            "[MF-DIAG] MFTEnumEx(HARDWARE) hr=0x%08lX count=%u\n",
            static_cast<unsigned long>(hr), count);
    fflush(stderr);

    for (UINT32 i = 0; i < count; ++i) {
      WCHAR* friendly_name = nullptr;
      UINT32 name_len = 0;
      if (SUCCEEDED(activates[i]->GetAllocatedString(
              MFT_FRIENDLY_NAME_Attribute, &friendly_name, &name_len))) {
        fwprintf(stderr, L"[MF-DIAG]   MFT[%u] friendly name: %ls\n", i,
                 friendly_name);
        CoTaskMemFree(friendly_name);
      } else {
        fprintf(stderr, "[MF-DIAG]   MFT[%u] (no friendly name available)\n", i);
      }
    }
    fflush(stderr);

    IMFActivate** all_activates = nullptr;
    UINT32 all_count = 0;
    const HRESULT all_hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &output_type_info, &all_activates, &all_count);
    fprintf(stderr, "[MF-DIAG] MFTEnumEx(ALL, no hw filter) hr=0x%08lX count=%u\n",
            static_cast<unsigned long>(all_hr), all_count);
    fflush(stderr);
    for (UINT32 i = 0; i < all_count; ++i) {
      WCHAR* friendly_name = nullptr;
      UINT32 name_len = 0;
      if (SUCCEEDED(all_activates[i]->GetAllocatedString(
              MFT_FRIENDLY_NAME_Attribute, &friendly_name, &name_len))) {
        fwprintf(stderr, L"[MF-DIAG]   ALL-MFT[%u] friendly name: %ls\n", i,
                 friendly_name);
        CoTaskMemFree(friendly_name);
      }
      all_activates[i]->Release();
    }
    if (all_activates) {
      CoTaskMemFree(all_activates);
    }

    supported = SUCCEEDED(hr) && count > 0;

    for (UINT32 i = 0; i < count; ++i) {
      activates[i]->Release();
    }
    if (activates) {
      CoTaskMemFree(activates);
    }

    MFShutdown();
  } else {
    fprintf(stderr, "[MF-DIAG] MFStartup FAILED, treating as unsupported\n");
    fflush(stderr);
    RTC_LOG(LS_WARNING)
        << "MFStartup failed while probing for hardware H264 encoder "
        << "support; assuming unsupported";
  }

  if (co_owned) {
    CoUninitialize();
  }

  fprintf(stderr, "[MF-DIAG] ProbeHardwareH264Encoder() returning %d\n", supported);
  fflush(stderr);

  return supported;
}

}

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
  static const bool kSupported = ProbeHardwareH264Encoder();
  fprintf(stderr, "[MF-DIAG] IsSupported() called, returning %d\n", kSupported);
  fflush(stderr);
  return kSupported;
}

std::unique_ptr<VideoEncoder> MediaFoundationVideoEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  fprintf(stderr, "[MF-DIAG] Create() called for format name=%s\n",
          format.name.c_str());
  fflush(stderr);

  if (!IsSupported()) {
    fprintf(stderr, "[MF-DIAG] Create() returning nullptr: not supported\n");
    fflush(stderr);
    return nullptr;
  }

  for (const auto& supported_format : supported_formats_) {
    if (format.IsSameCodec(supported_format)) {
      if (format.name == "H264") {
        fprintf(stderr, "[MF-DIAG] Create() returning MediaFoundationH264EncoderImpl\n");
        fflush(stderr);
        RTC_LOG(LS_INFO) << "Using Media Foundation HW encoder for H264";
        return std::make_unique<MediaFoundationH264EncoderImpl>(env, format);
      }
    }
  }
  fprintf(stderr, "[MF-DIAG] Create() returning nullptr: no format match\n");
  fflush(stderr);
  return nullptr;
}

std::vector<SdpVideoFormat>
MediaFoundationVideoEncoderFactory::GetSupportedFormats() const {
  return supported_formats_;
}

std::vector<SdpVideoFormat>
MediaFoundationVideoEncoderFactory::GetImplementations() const {
  return supported_formats_;
}

}