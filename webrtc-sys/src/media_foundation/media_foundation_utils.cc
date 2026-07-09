#include "media_foundation_utils.h"

#include <mferror.h>
#include <wrl/client.h>

#include <cstring>
#include <sstream>

#include "common_video/h264/h264_common.h"
#include "rtc_base/logging.h"
#include "libyuv/convert.h"

namespace webrtc {
namespace mf_utils {

std::string HResultToString(HRESULT hr) {
  std::stringstream ss;
  ss << "0x" << std::hex << std::uppercase << hr;
  return ss.str();
}

bool ConvertI420ToNV12(const VideoFrame& frame, std::vector<uint8_t>* nv12_out) {
  webrtc::scoped_refptr<I420BufferInterface> i420 =
      frame.video_frame_buffer()->ToI420();
  if (!i420) {
    RTC_LOG(LS_ERROR) << "Failed to obtain I420 buffer for NV12 conversion";
    return false;
  }

  const int width = i420->width();
  const int height = i420->height();
  const int uv_stride = 2 * ((width + 1) / 2);
  const size_t nv12_size = static_cast<size_t>(width) * height +
                           static_cast<size_t>(uv_stride) * ((height + 1) / 2);

  nv12_out->resize(nv12_size);

  uint8_t* y_dst = nv12_out->data();
  uint8_t* uv_dst = y_dst + static_cast<size_t>(width) * height;

  const int result = libyuv::I420ToNV12(
      i420->DataY(), i420->StrideY(), i420->DataU(), i420->StrideU(),
      i420->DataV(), i420->StrideV(), y_dst, width, uv_dst, uv_stride, width,
      height);

  if (result != 0) {
    RTC_LOG(LS_ERROR) << "libyuv::I420ToNV12 failed, code=" << result;
    return false;
  }
  return true;
}

HRESULT CreateNV12Sample(const uint8_t* nv12_data,
                         size_t nv12_size,
                         int width,
                         int height,
                         LONGLONG timestamp_100ns,
                         LONGLONG duration_100ns,
                         IMFSample** out_sample) {
  if (!out_sample) {
    return E_POINTER;
  }
  *out_sample = nullptr;

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(nv12_size), &buffer);
  if (FAILED(hr)) {
    return hr;
  }

  BYTE* dst = nullptr;
  DWORD max_len = 0;
  hr = buffer->Lock(&dst, &max_len, nullptr);
  if (FAILED(hr)) {
    return hr;
  }
  if (static_cast<size_t>(max_len) < nv12_size) {
    buffer->Unlock();
    return MF_E_BUFFERTOOSMALL;
  }
  std::memcpy(dst, nv12_data, nv12_size);
  buffer->Unlock();

  hr = buffer->SetCurrentLength(static_cast<DWORD>(nv12_size));
  if (FAILED(hr)) {
    return hr;
  }

  Microsoft::WRL::ComPtr<IMFSample> sample;
  hr = MFCreateSample(&sample);
  if (FAILED(hr)) {
    return hr;
  }

  hr = sample->AddBuffer(buffer.Get());
  if (FAILED(hr)) {
    return hr;
  }

  hr = sample->SetSampleTime(timestamp_100ns);
  if (FAILED(hr)) {
    return hr;
  }

  hr = sample->SetSampleDuration(duration_100ns);
  if (FAILED(hr)) {
    return hr;
  }

  *out_sample = sample.Detach();
  return S_OK;
}

HRESULT CreateEmptyOutputSample(DWORD min_size, IMFSample** out_sample) {
  if (!out_sample) {
    return E_POINTER;
  }
  *out_sample = nullptr;

  Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateMemoryBuffer(min_size, &buffer);
  if (FAILED(hr)) {
    return hr;
  }

  Microsoft::WRL::ComPtr<IMFSample> sample;
  hr = MFCreateSample(&sample);
  if (FAILED(hr)) {
    return hr;
  }

  hr = sample->AddBuffer(buffer.Get());
  if (FAILED(hr)) {
    return hr;
  }

  *out_sample = sample.Detach();
  return S_OK;
}

eAVEncH264VProfile H264ProfileToMFProfile(H264Profile profile) {
  switch (profile) {
    case H264Profile::kProfileConstrainedBaseline:
    case H264Profile::kProfileBaseline:
      return eAVEncH264VProfile_Base;
    case H264Profile::kProfileMain:
      return eAVEncH264VProfile_Main;
    case H264Profile::kProfileConstrainedHigh:
    case H264Profile::kProfileHigh:
      return eAVEncH264VProfile_High;
    default:
      return eAVEncH264VProfile_Base;
  }
}

bool ContainsIdrNalu(const uint8_t* data, size_t size) {
  if (!data || size == 0) {
    return false;
  }
  std::vector<H264::NaluIndex> indices =
      H264::FindNaluIndices(webrtc::MakeArrayView(data, size));
  for (const auto& index : indices) {
    if (index.payload_size == 0) {
      continue;
    }
    const H264::NaluType type = H264::ParseNaluType(data[index.payload_start_offset]);
    if (type == H264::NaluType::kIdr) {
      return true;
    }
  }
  return false;
}

}
}