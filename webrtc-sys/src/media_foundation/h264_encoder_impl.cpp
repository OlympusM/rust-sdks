#include "h264_encoder_impl.h"

#include <mferror.h>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>

#include "absl/strings/match.h"
#include "absl/types/optional.h"
#include "api/video/video_codec_constants.h"
#include "common_video/h264/h264_common.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "media_foundation_utils.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace webrtc {

namespace {

class MediaFoundationRuntime {
 public:
  static bool Acquire() {
    std::lock_guard<std::mutex> lock(Mutex());
    if (RefCount() == 0) {
      const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
      if (FAILED(hr)) {
        RTC_LOG(LS_ERROR) << "MFStartup failed: "
                          << mf_utils::HResultToString(hr);
        return false;
      }
    }
    ++RefCount();
    return true;
  }

  static void Release() {
    std::lock_guard<std::mutex> lock(Mutex());
    if (RefCount() > 0 && --RefCount() == 0) {
      MFShutdown();
    }
  }

 private:
  static std::mutex& Mutex() {
    static std::mutex* mutex = new std::mutex();
    return *mutex;
  }
  static int& RefCount() {
    static int ref_count = 0;
    return ref_count;
  }
};

}

MediaFoundationH264EncoderImpl::MediaFoundationH264EncoderImpl(
    const webrtc::Environment& env,
    const SdpVideoFormat& format)
    : env_(env),
      packetization_mode_(
          H264EncoderSettings::Parse(format).packetization_mode),
      format_(format) {
  auto it = format_.parameters.find("profile-level-id");
  if (it != format_.parameters.end()) {
    std::optional<webrtc::H264ProfileLevelId> profile_level_id =
        webrtc::ParseH264ProfileLevelId(it->second.c_str());
    if (profile_level_id.has_value()) {
      profile_ = profile_level_id->profile;
    }
  } else {
    RTC_LOG(LS_WARNING)
        << "H264 format missing profile-level-id, defaulting to "
        << "constrained baseline";
  }
}

MediaFoundationH264EncoderImpl::~MediaFoundationH264EncoderImpl() {
  Release();
  if (com_initialized_) {
    CoUninitialize();
    com_initialized_ = false;
  }
}

bool MediaFoundationH264EncoderImpl::CreateEncoderMFT() {
  fprintf(stderr, "[MF-DIAG-IMPL] CreateEncoderMFT() starting\n");
  fflush(stderr);

  if (!com_initialized_) {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
      com_initialized_ = true;
    } else if (hr == RPC_E_CHANGED_MODE) {

      RTC_LOG(LS_WARNING)
          << "COM apartment already initialized in a different mode; "
          << "proceeding without owning CoUninitialize()";
    } else {
      fprintf(stderr, "[MF-DIAG-IMPL] CoInitializeEx FAILED hr=0x%08lX\n",
              static_cast<unsigned long>(hr));
      fflush(stderr);
      RTC_LOG(LS_ERROR) << "CoInitializeEx failed: "
                        << mf_utils::HResultToString(hr);
      return false;
    }
  }

  if (!mf_runtime_acquired_) {
    if (!MediaFoundationRuntime::Acquire()) {
      fprintf(stderr, "[MF-DIAG-IMPL] MediaFoundationRuntime::Acquire() FAILED\n");
      fflush(stderr);
      return false;
    }
    mf_runtime_acquired_ = true;
  }

  MFT_REGISTER_TYPE_INFO output_type_info = {MFMediaType_Video,
                                             MFVideoFormat_H264};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;

  const HRESULT enum_hr = MFTEnumEx(
      MFT_CATEGORY_VIDEO_ENCODER,
      MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
      nullptr,
      &output_type_info,
      &activates, &count);

  fprintf(stderr, "[MF-DIAG-IMPL] MFTEnumEx hr=0x%08lX count=%u\n",
          static_cast<unsigned long>(enum_hr), count);
  fflush(stderr);

  if (FAILED(enum_hr) || count == 0) {
    RTC_LOG(LS_WARNING) << "No hardware H264 encoder MFT found ("
                        << mf_utils::HResultToString(enum_hr) << ")";
    for (UINT32 i = 0; i < count; ++i) {
      activates[i]->Release();
    }
    if (activates) {
      CoTaskMemFree(activates);
    }
    return false;
  }

  Microsoft::WRL::ComPtr<IMFTransform> transform;
  const HRESULT activate_hr =
      activates[0]->ActivateObject(IID_PPV_ARGS(&transform));

  fprintf(stderr, "[MF-DIAG-IMPL] ActivateObject hr=0x%08lX transform=%p\n",
          static_cast<unsigned long>(activate_hr), (void*)transform.Get());
  fflush(stderr);

  for (UINT32 i = 0; i < count; ++i) {
    activates[i]->Release();
  }
  CoTaskMemFree(activates);

  if (FAILED(activate_hr) || !transform) {
    RTC_LOG(LS_WARNING) << "Failed to activate hardware H264 encoder MFT: "
                        << mf_utils::HResultToString(activate_hr);
    return false;
  }

  transform_ = transform;

  const HRESULT qi_hr = transform_.As(&codec_api_);
  fprintf(stderr, "[MF-DIAG-IMPL] QueryInterface(ICodecAPI) hr=0x%08lX\n",
          static_cast<unsigned long>(qi_hr));
  fflush(stderr);

  if (FAILED(qi_hr)) {
    RTC_LOG(LS_WARNING) << "Encoder MFT does not expose ICodecAPI: "
                        << mf_utils::HResultToString(qi_hr);
    transform_.Reset();
    return false;
  }

  if (!UnlockAsyncTransformIfNeeded()) {
    RTC_LOG(LS_ERROR) << "Failed to query/unlock async transform state";
    transform_.Reset();
    codec_api_.Reset();
    return false;
  }

  fprintf(stderr, "[MF-DIAG-IMPL] CreateEncoderMFT() succeeded, is_async_=%d\n",
          is_async_);
  fflush(stderr);
  return true;
}

bool MediaFoundationH264EncoderImpl::UnlockAsyncTransformIfNeeded() {
  Microsoft::WRL::ComPtr<IMFAttributes> attributes;
  const HRESULT attr_hr = transform_->GetAttributes(&attributes);
  fprintf(stderr, "[MF-DIAG-IMPL] GetAttributes hr=0x%08lX\n",
          static_cast<unsigned long>(attr_hr));
  fflush(stderr);
  if (FAILED(attr_hr)) {
    is_async_ = false;
    return true;
  }

  UINT32 async_flag = 0;
  attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async_flag);
  fprintf(stderr, "[MF-DIAG-IMPL] MF_TRANSFORM_ASYNC=%u\n", async_flag);
  fflush(stderr);

  if (!async_flag) {
    is_async_ = false;
    return true;
  }

  const HRESULT unlock_hr =
      attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
  fprintf(stderr, "[MF-DIAG-IMPL] SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK) hr=0x%08lX\n",
          static_cast<unsigned long>(unlock_hr));
  fflush(stderr);
  if (FAILED(unlock_hr)) {
    return false;
  }

  const HRESULT qi_hr = transform_.As(&event_generator_);
  fprintf(stderr, "[MF-DIAG-IMPL] QueryInterface(IMFMediaEventGenerator) hr=0x%08lX\n",
          static_cast<unsigned long>(qi_hr));
  fflush(stderr);
  if (FAILED(qi_hr)) {
    return false;
  }

  is_async_ = true;
  async_need_input_count_ = 0;
  return true;
}

bool MediaFoundationH264EncoderImpl::ConfigureMediaTypes(
    int width, int height, int fps, uint32_t bitrate_bps) {
  HRESULT hr;

  Microsoft::WRL::ComPtr<IMFMediaType> output_type;
  hr = MFCreateMediaType(&output_type);
  if (FAILED(hr)) {
    return false;
  }
  output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, width, height);
  MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, fps, 1);
  MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  output_type->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps);
  output_type->SetUINT32(MF_MT_MPEG2_PROFILE,
                         mf_utils::H264ProfileToMFProfile(profile_));

  hr = transform_->SetOutputType(0, output_type.Get(), 0);
  fprintf(stderr, "[MF-DIAG-IMPL] SetOutputType(H264) hr=0x%08lX\n",
          static_cast<unsigned long>(hr));
  fflush(stderr);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "SetOutputType failed: "
                      << mf_utils::HResultToString(hr);
    return false;
  }

  Microsoft::WRL::ComPtr<IMFMediaType> input_type;
  hr = MFCreateMediaType(&input_type);
  if (FAILED(hr)) {
    return false;
  }
  input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, width, height);
  MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, fps, 1);
  MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

  hr = transform_->SetInputType(0, input_type.Get(), 0);
  fprintf(stderr, "[MF-DIAG-IMPL] SetInputType(NV12) hr=0x%08lX\n",
          static_cast<unsigned long>(hr));
  fflush(stderr);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "SetInputType failed: "
                      << mf_utils::HResultToString(hr);
    return false;
  }

  MFT_OUTPUT_STREAM_INFO stream_info = {};
  hr = transform_->GetOutputStreamInfo(0, &stream_info);
  if (FAILED(hr)) {
    fprintf(stderr, "[MF-DIAG-IMPL] GetOutputStreamInfo FAILED hr=0x%08lX\n",
            static_cast<unsigned long>(hr));
    fflush(stderr);
    RTC_LOG(LS_ERROR) << "GetOutputStreamInfo failed: "
                      << mf_utils::HResultToString(hr);
    return false;
  }

  output_provides_samples_ =
      (stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                              MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
  output_sample_min_size_ = stream_info.cbSize;
  output_sample_alignment_ = stream_info.cbAlignment;

  fprintf(stderr,
          "[MF-DIAG-IMPL] ConfigureMediaTypes SUCCESS: "
          "output_provides_samples=%d min_size=%lu alignment=%lu\n",
          output_provides_samples_,
          static_cast<unsigned long>(output_sample_min_size_),
          static_cast<unsigned long>(output_sample_alignment_));
  fflush(stderr);

  return true;
}

bool MediaFoundationH264EncoderImpl::ConfigureCodecApi(uint32_t bitrate_bps,
                                                       int fps) {
  if (!codec_api_) {
    return false;
  }

  auto set_value = [this](const GUID& api, VARIANT* value, const char* name) {
    const HRESULT hr = codec_api_->SetValue(&api, value);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "ICodecAPI: " << name << " not accepted ("
                          << mf_utils::HResultToString(hr) << ")";
    }
  };

  VARIANT var;
  VariantInit(&var);
  var.vt = VT_UI4;
  var.ulVal = eAVEncCommonRateControlMode_CBR;
  set_value(CODECAPI_AVEncCommonRateControlMode, &var,
           "AVEncCommonRateControlMode");

  VariantInit(&var);
  var.vt = VT_UI4;
  var.ulVal = bitrate_bps;
  set_value(CODECAPI_AVEncCommonMeanBitRate, &var, "AVEncCommonMeanBitRate");

  VariantInit(&var);
  var.vt = VT_BOOL;
  var.boolVal = VARIANT_TRUE;
  set_value(CODECAPI_AVLowLatencyMode, &var, "AVLowLatencyMode");

  if (profile_ == H264Profile::kProfileMain ||
      profile_ == H264Profile::kProfileHigh ||
      profile_ == H264Profile::kProfileConstrainedHigh) {
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = VARIANT_TRUE;
    set_value(CODECAPI_AVEncH264CABACEnable, &var, "AVEncH264CABACEnable");
  }

  return true;
}

void MediaFoundationH264EncoderImpl::DestroyEncoder() {
  if (transform_) {
    if (streaming_started_) {
      transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
      transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    codec_api_.Reset();
    transform_.Reset();
  }
  event_generator_.Reset();
  is_async_ = false;
  async_need_input_count_ = 0;
  pending_frames_.clear();
  streaming_started_ = false;
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

  if (transform_) {
    DestroyEncoder();
  }

  codec_ = *inst;
  frame_count_ = 0;

  const size_t new_capacity =
      CalcBufferSize(VideoType::kI420, codec_.width, codec_.height);
  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(new_capacity));
  encoded_image_._encodedWidth = codec_.width;
  encoded_image_._encodedHeight = codec_.height;
  encoded_image_.set_size(0);

  if (!CreateEncoderMFT()) {
    fprintf(stderr, "[MF-DIAG-IMPL] InitEncode: CreateEncoderMFT FAILED, returning WEBRTC_VIDEO_CODEC_ERROR\n");
    fflush(stderr);
    RTC_LOG(LS_WARNING)
        << "Media Foundation: no usable hardware H264 encoder available";
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  const uint32_t bitrate_bps =
      (codec_.maxBitrate > 0 ? codec_.maxBitrate : codec_.startBitrate) *
      1000;

  if (!ConfigureMediaTypes(codec_.width, codec_.height,
                          static_cast<int>(codec_.maxFramerate),
                          bitrate_bps)) {
    fprintf(stderr, "[MF-DIAG-IMPL] InitEncode: ConfigureMediaTypes FAILED, returning WEBRTC_VIDEO_CODEC_ERROR\n");
    fflush(stderr);
    DestroyEncoder();
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (!ConfigureCodecApi(bitrate_bps, static_cast<int>(codec_.maxFramerate))) {
    RTC_LOG(LS_WARNING)
        << "Media Foundation: ICodecAPI configuration incomplete; "
        << "continuing with driver defaults for unset properties";
  }

  HRESULT hr = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "Initial FLUSH failed: "
                        << mf_utils::HResultToString(hr);
  }
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  streaming_started_ = true;

  fprintf(stderr, "[MF-DIAG-IMPL] InitEncode: SUCCESS, returning WEBRTC_VIDEO_CODEC_OK\n");
  fflush(stderr);

  RTC_LOG(LS_INFO) << "Media Foundation H264 encoder initialized: "
                   << codec_.width << "x" << codec_.height << " @ "
                   << codec_.maxFramerate << "fps, " << bitrate_bps
                   << " bps";

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MediaFoundationH264EncoderImpl::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_image_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t MediaFoundationH264EncoderImpl::Release() {
  DestroyEncoder();
  return WEBRTC_VIDEO_CODEC_OK;
}

HRESULT MediaFoundationH264EncoderImpl::SubmitInput(const VideoFrame& frame,
                                                    bool force_keyframe) {
  const bool diag = frame_count_ < 10;

  if (!mf_utils::ConvertI420ToNV12(frame, &nv12_buffer_)) {
    fprintf(stderr, "[MF-DIAG-IMPL] frame=%lld ConvertI420ToNV12 FAILED\n",
            static_cast<long long>(frame_count_));
    fflush(stderr);
    return E_FAIL;
  }

  if (force_keyframe && codec_api_) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = VARIANT_TRUE;
    const HRESULT hr =
        codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "Failed to force keyframe: "
                          << mf_utils::HResultToString(hr);
    }
  }

  const LONGLONG timestamp_100ns =
      static_cast<LONGLONG>(frame.timestamp_us()) * 10;
  const LONGLONG duration_100ns =
      codec_.maxFramerate > 0
          ? static_cast<LONGLONG>(10000000LL / codec_.maxFramerate)
          : 0;

  Microsoft::WRL::ComPtr<IMFSample> sample;
  const HRESULT create_hr = mf_utils::CreateNV12Sample(
      nv12_buffer_.data(), nv12_buffer_.size(), codec_.width, codec_.height,
      timestamp_100ns, duration_100ns, &sample);
  if (FAILED(create_hr)) {
    fprintf(stderr, "[MF-DIAG-IMPL] frame=%lld CreateNV12Sample FAILED hr=0x%08lX\n",
            static_cast<long long>(frame_count_),
            static_cast<unsigned long>(create_hr));
    fflush(stderr);
    RTC_LOG(LS_ERROR) << "Failed to build input sample: "
                      << mf_utils::HResultToString(create_hr);
    return create_hr;
  }

  const HRESULT process_hr = transform_->ProcessInput(0, sample.Get(), 0);
  if (diag) {
    fprintf(stderr,
            "[MF-DIAG-IMPL] frame=%lld ProcessInput hr=0x%08lX nv12_bytes=%zu\n",
            static_cast<long long>(frame_count_),
            static_cast<unsigned long>(process_hr), nv12_buffer_.size());
    fflush(stderr);
  }
  return process_hr;
}

bool MediaFoundationH264EncoderImpl::HandleStreamChange() {
  Microsoft::WRL::ComPtr<IMFMediaType> new_output_type;
  HRESULT hr = transform_->GetOutputAvailableType(0, 0, &new_output_type);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "GetOutputAvailableType failed after stream "
                      << "change: " << mf_utils::HResultToString(hr);
    return false;
  }

  hr = transform_->SetOutputType(0, new_output_type.Get(), 0);
  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "SetOutputType failed after stream change: "
                      << mf_utils::HResultToString(hr);
    return false;
  }

  MFT_OUTPUT_STREAM_INFO stream_info = {};
  hr = transform_->GetOutputStreamInfo(0, &stream_info);
  if (SUCCEEDED(hr)) {
    output_provides_samples_ =
        (stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    output_sample_min_size_ = stream_info.cbSize;
    output_sample_alignment_ = stream_info.cbAlignment;
  }
  return true;
}

bool MediaFoundationH264EncoderImpl::DrainOutput(
    const VideoFrame& source_frame) {
  for (;;) {
    MFT_OUTPUT_DATA_BUFFER output_buffer = {};
    Microsoft::WRL::ComPtr<IMFSample> owned_output_sample;

    if (!output_provides_samples_) {
      const DWORD min_size = std::max<DWORD>(
          output_sample_min_size_, static_cast<DWORD>(nv12_buffer_.size()));
      const HRESULT alloc_hr = mf_utils::CreateEmptyOutputSample(
          min_size, &owned_output_sample);
      if (FAILED(alloc_hr)) {
        RTC_LOG(LS_ERROR) << "Failed to allocate output sample: "
                          << mf_utils::HResultToString(alloc_hr);
        return false;
      }
      output_buffer.pSample = owned_output_sample.Get();
    }

    DWORD status = 0;
    const HRESULT hr = transform_->ProcessOutput(0, 1, &output_buffer, &status);

    if (frame_count_ < 10) {
      fprintf(stderr,
              "[MF-DIAG-IMPL] frame=%lld ProcessOutput hr=0x%08lX status=%lu\n",
              static_cast<long long>(frame_count_),
              static_cast<unsigned long>(hr),
              static_cast<unsigned long>(status));
      fflush(stderr);
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return true;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      if (output_provides_samples_ && output_buffer.pSample) {
        output_buffer.pSample->Release();
      }
      if (!HandleStreamChange()) {
        return false;
      }
      continue;
    }

    if (FAILED(hr)) {
      if (output_provides_samples_ && output_buffer.pSample) {
        output_buffer.pSample->Release();
      }
      RTC_LOG(LS_ERROR) << "ProcessOutput failed: "
                        << mf_utils::HResultToString(hr);
      return false;
    }

    IMFSample* result_sample = output_provides_samples_
                                   ? output_buffer.pSample
                                   : owned_output_sample.Get();
    if (!result_sample) {
      continue;
    }

    UINT32 clean_point = 0;
    result_sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point);

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    const HRESULT convert_hr = result_sample->ConvertToContiguousBuffer(&buffer);

    if (output_provides_samples_ && output_buffer.pSample) {
      output_buffer.pSample->Release();
    }

    if (FAILED(convert_hr)) {
      RTC_LOG(LS_ERROR) << "ConvertToContiguousBuffer failed: "
                        << mf_utils::HResultToString(convert_hr);
      return false;
    }

    BYTE* data = nullptr;
    DWORD length = 0;
    const HRESULT lock_hr = buffer->Lock(&data, nullptr, &length);
    if (FAILED(lock_hr)) {
      RTC_LOG(LS_ERROR) << "Failed to lock output buffer: "
                        << mf_utils::HResultToString(lock_hr);
      return false;
    }

    encoded_image_.SetEncodedData(EncodedImageBuffer::Create(data, length));
    buffer->Unlock();

    const bool is_keyframe =
        clean_point != 0 ||
        mf_utils::ContainsIdrNalu(encoded_image_.data(), encoded_image_.size());

    encoded_image_._frameType = is_keyframe ? VideoFrameType::kVideoFrameKey
                                            : VideoFrameType::kVideoFrameDelta;
    encoded_image_._encodedWidth = codec_.width;
    encoded_image_._encodedHeight = codec_.height;
    encoded_image_.SetRtpTimestamp(source_frame.rtp_timestamp());
    encoded_image_.capture_time_ms_ = source_frame.render_time_ms();
    encoded_image_.SetColorSpace(source_frame.color_space());

    h264_bitstream_parser_.ParseBitstream(encoded_image_);
    const auto qp = h264_bitstream_parser_.GetLastSliceQp();
    encoded_image_.qp_ = qp.value_or(-1);

    CodecSpecificInfo codec_specific;
    codec_specific.codecType = kVideoCodecH264;
    codec_specific.codecSpecific.H264.packetization_mode = packetization_mode_;
    codec_specific.codecSpecific.H264.idr_frame = is_keyframe;

    if (frame_count_ < 10) {
      fprintf(stderr,
              "[MF-DIAG-IMPL] frame=%lld DELIVERING encoded frame, size=%zu "
              "keyframe=%d\n",
              static_cast<long long>(frame_count_), encoded_image_.size(),
              is_keyframe);
      fflush(stderr);
    }

    encoded_image_callback_->OnEncodedImage(encoded_image_, &codec_specific);
  }
}

bool MediaFoundationH264EncoderImpl::DeliverNextOutputSample() {
  MFT_OUTPUT_DATA_BUFFER output_buffer = {};
  Microsoft::WRL::ComPtr<IMFSample> owned_output_sample;

  if (!output_provides_samples_) {
    const DWORD min_size = std::max<DWORD>(
        output_sample_min_size_, static_cast<DWORD>(nv12_buffer_.size()));
    const HRESULT alloc_hr =
        mf_utils::CreateEmptyOutputSample(min_size, &owned_output_sample);
    if (FAILED(alloc_hr)) {
      RTC_LOG(LS_ERROR) << "Failed to allocate output sample: "
                        << mf_utils::HResultToString(alloc_hr);
      return false;
    }
    output_buffer.pSample = owned_output_sample.Get();
  }

  DWORD status = 0;
  const HRESULT hr = transform_->ProcessOutput(0, 1, &output_buffer, &status);

  if (frame_count_ < 10) {
    fprintf(stderr,
            "[MF-DIAG-IMPL] frame=%lld (async) ProcessOutput hr=0x%08lX status=%lu\n",
            static_cast<long long>(frame_count_),
            static_cast<unsigned long>(hr), static_cast<unsigned long>(status));
    fflush(stderr);
  }

  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
    return true;
  }

  if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
    if (output_provides_samples_ && output_buffer.pSample) {
      output_buffer.pSample->Release();
    }
    return HandleStreamChange();
  }

  if (FAILED(hr)) {
    if (output_provides_samples_ && output_buffer.pSample) {
      output_buffer.pSample->Release();
    }
    RTC_LOG(LS_ERROR) << "ProcessOutput (async) failed: "
                      << mf_utils::HResultToString(hr);
    return false;
  }

  IMFSample* result_sample =
      output_provides_samples_ ? output_buffer.pSample : owned_output_sample.Get();
  if (!result_sample) {
    return true;
  }

  UINT32 clean_point = 0;
  result_sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point);

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  const HRESULT convert_hr = result_sample->ConvertToContiguousBuffer(&buffer);

  if (output_provides_samples_ && output_buffer.pSample) {
    output_buffer.pSample->Release();
  }

  if (FAILED(convert_hr)) {
    RTC_LOG(LS_ERROR) << "ConvertToContiguousBuffer (async) failed: "
                      << mf_utils::HResultToString(convert_hr);
    return false;
  }

  BYTE* data = nullptr;
  DWORD length = 0;
  const HRESULT lock_hr = buffer->Lock(&data, nullptr, &length);
  if (FAILED(lock_hr)) {
    RTC_LOG(LS_ERROR) << "Failed to lock output buffer (async): "
                      << mf_utils::HResultToString(lock_hr);
    return false;
  }

  encoded_image_.SetEncodedData(EncodedImageBuffer::Create(data, length));
  buffer->Unlock();

  const bool is_keyframe =
      clean_point != 0 ||
      mf_utils::ContainsIdrNalu(encoded_image_.data(), encoded_image_.size());

  PendingFrameInfo frame_info;
  if (!pending_frames_.empty()) {
    frame_info = pending_frames_.front();
    pending_frames_.pop_front();
  } else {
    fprintf(stderr,
            "[MF-DIAG-IMPL] WARNING: output delivered with empty "
            "pending_frames_ queue\n");
    fflush(stderr);
  }

  encoded_image_._frameType = is_keyframe ? VideoFrameType::kVideoFrameKey
                                          : VideoFrameType::kVideoFrameDelta;
  encoded_image_._encodedWidth = codec_.width;
  encoded_image_._encodedHeight = codec_.height;
  encoded_image_.SetRtpTimestamp(frame_info.rtp_timestamp);
  encoded_image_.capture_time_ms_ = frame_info.capture_time_ms;
  encoded_image_.SetColorSpace(frame_info.color_space);

  h264_bitstream_parser_.ParseBitstream(encoded_image_);
  const auto qp = h264_bitstream_parser_.GetLastSliceQp();
  encoded_image_.qp_ = qp.value_or(-1);

  CodecSpecificInfo codec_specific;
  codec_specific.codecType = kVideoCodecH264;
  codec_specific.codecSpecific.H264.packetization_mode = packetization_mode_;
  codec_specific.codecSpecific.H264.idr_frame = is_keyframe;

  if (frame_count_ < 10) {
    fprintf(stderr,
            "[MF-DIAG-IMPL] frame=%lld (async) DELIVERING encoded frame, "
            "size=%zu keyframe=%d\n",
            static_cast<long long>(frame_count_), encoded_image_.size(),
            is_keyframe);
    fflush(stderr);
  }

  encoded_image_callback_->OnEncodedImage(encoded_image_, &codec_specific);
  return true;
}

bool MediaFoundationH264EncoderImpl::HandleAsyncEvent(IMFMediaEvent* event) {
  MediaEventType type = MEUnknown;
  const HRESULT type_hr = event->GetType(&type);
  if (FAILED(type_hr)) {
    RTC_LOG(LS_ERROR) << "IMFMediaEvent::GetType failed: "
                      << mf_utils::HResultToString(type_hr);
    return false;
  }

  if (frame_count_ < 10) {
    fprintf(stderr, "[MF-DIAG-IMPL] frame=%lld async event type=%d\n",
            static_cast<long long>(frame_count_), static_cast<int>(type));
    fflush(stderr);
  }

  switch (type) {
    case METransformNeedInput:
      ++async_need_input_count_;
      return true;
    case METransformHaveOutput:
      return DeliverNextOutputSample();
    case METransformDrainComplete:
      return true;
    default:
      return true;
  }
}

bool MediaFoundationH264EncoderImpl::PumpAvailableEventsNonBlocking() {
  for (;;) {
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    const HRESULT hr =
        event_generator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) {
      return true;
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "GetEvent (non-blocking) failed: "
                        << mf_utils::HResultToString(hr);
      return false;
    }
    if (!HandleAsyncEvent(event.Get())) {
      return false;
    }
  }
}

bool MediaFoundationH264EncoderImpl::SubmitInputAsync(const VideoFrame& frame,
                                                      bool force_keyframe) {
  if (!mf_utils::ConvertI420ToNV12(frame, &nv12_buffer_)) {
    fprintf(stderr, "[MF-DIAG-IMPL] frame=%lld (async) ConvertI420ToNV12 FAILED\n",
            static_cast<long long>(frame_count_));
    fflush(stderr);
    return false;
  }

  if (force_keyframe && codec_api_) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = VARIANT_TRUE;
    codec_api_->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
  }

  while (async_need_input_count_ <= 0) {
    Microsoft::WRL::ComPtr<IMFMediaEvent> event;
    const HRESULT hr = event_generator_->GetEvent(0, &event);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "GetEvent (blocking) failed: "
                        << mf_utils::HResultToString(hr);
      return false;
    }
    if (!HandleAsyncEvent(event.Get())) {
      return false;
    }
  }

  const LONGLONG timestamp_100ns =
      static_cast<LONGLONG>(frame.timestamp_us()) * 10;
  const LONGLONG duration_100ns =
      codec_.maxFramerate > 0
          ? static_cast<LONGLONG>(10000000LL / codec_.maxFramerate)
          : 0;

  Microsoft::WRL::ComPtr<IMFSample> sample;
  const HRESULT create_hr = mf_utils::CreateNV12Sample(
      nv12_buffer_.data(), nv12_buffer_.size(), codec_.width, codec_.height,
      timestamp_100ns, duration_100ns, &sample);
  if (FAILED(create_hr)) {
    RTC_LOG(LS_ERROR) << "Failed to build input sample (async): "
                      << mf_utils::HResultToString(create_hr);
    return false;
  }

  const HRESULT process_hr = transform_->ProcessInput(0, sample.Get(), 0);
  if (frame_count_ < 10) {
    fprintf(stderr, "[MF-DIAG-IMPL] frame=%lld (async) ProcessInput hr=0x%08lX\n",
            static_cast<long long>(frame_count_),
            static_cast<unsigned long>(process_hr));
    fflush(stderr);
  }

  if (FAILED(process_hr)) {
    RTC_LOG(LS_ERROR) << "ProcessInput (async) failed: "
                      << mf_utils::HResultToString(process_hr);
    return false;
  }

  --async_need_input_count_;

  PendingFrameInfo frame_info;
  frame_info.rtp_timestamp = frame.rtp_timestamp();
  frame_info.capture_time_ms = frame.render_time_ms();
  frame_info.color_space = frame.color_space();
  pending_frames_.push_back(frame_info);

  return PumpAvailableEventsNonBlocking();
}

int32_t MediaFoundationH264EncoderImpl::Encode(
    const VideoFrame& input_frame,
    const std::vector<VideoFrameType>* frame_types) {
  if (!encoded_image_callback_) {
    fprintf(stderr, "[MF-DIAG-IMPL] Encode: no callback registered\n");
    fflush(stderr);
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (!transform_) {
    fprintf(stderr, "[MF-DIAG-IMPL] Encode: transform_ is null\n");
    fflush(stderr);
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  bool force_keyframe = false;
  if (frame_types) {
    for (VideoFrameType type : *frame_types) {
      if (type == VideoFrameType::kVideoFrameKey) {
        force_keyframe = true;
        break;
      }
    }
  }

  if (is_async_) {
    if (!SubmitInputAsync(input_frame, force_keyframe)) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    ++frame_count_;
    return WEBRTC_VIDEO_CODEC_OK;
  }

  HRESULT hr = SubmitInput(input_frame, force_keyframe);

  if (hr == MF_E_NOTACCEPTING) {
    if (!DrainOutput(input_frame)) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    hr = SubmitInput(input_frame, force_keyframe);
  }

  if (FAILED(hr)) {
    RTC_LOG(LS_ERROR) << "ProcessInput failed: "
                      << mf_utils::HResultToString(hr);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  if (!DrainOutput(input_frame)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  ++frame_count_;
  return WEBRTC_VIDEO_CODEC_OK;
}

void MediaFoundationH264EncoderImpl::SetRates(
    const RateControlParameters& parameters) {
  if (parameters.framerate_fps < 1.0) {
    RTC_LOG(LS_WARNING) << "Invalid frame rate: " << parameters.framerate_fps;
    return;
  }

  codec_.maxFramerate = static_cast<uint32_t>(parameters.framerate_fps);
  const uint32_t bitrate_bps = parameters.bitrate.GetSpatialLayerSum(0);
  codec_.maxBitrate = bitrate_bps / 1000;

  if (codec_api_) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = bitrate_bps;
    const HRESULT hr =
        codec_api_->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "Live bitrate update failed: "
                          << mf_utils::HResultToString(hr);
    }
  }

  RTC_LOG(LS_INFO) << "Media Foundation H264 SetRates: "
                   << parameters.framerate_fps << "fps, " << bitrate_bps
                   << " bps";
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

}