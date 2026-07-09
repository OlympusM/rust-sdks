#ifndef WEBRTC_MEDIA_FOUNDATION_UTILS_H_
#define WEBRTC_MEDIA_FOUNDATION_UTILS_H_

#include <codecapi.h>
#include <mfapi.h>
#include <mfidl.h>

#include <cstdint>
#include <string>
#include <vector>

#include "api/video/video_frame.h"
#include "modules/video_coding/codecs/h264/include/h264.h"

namespace webrtc {
namespace mf_utils {

bool ConvertI420ToNV12(const VideoFrame& frame, std::vector<uint8_t>* nv12_out);

HRESULT CreateNV12Sample(const uint8_t* nv12_data,
                         size_t nv12_size,
                         int width,
                         int height,
                         LONGLONG timestamp_100ns,
                         LONGLONG duration_100ns,
                         IMFSample** out_sample);
HRESULT CreateEmptyOutputSample(DWORD min_size, IMFSample** out_sample);

eAVEncH264VProfile H264ProfileToMFProfile(H264Profile profile);

bool ContainsIdrNalu(const uint8_t* data, size_t size);

std::string HResultToString(HRESULT hr);

}
}

#endif