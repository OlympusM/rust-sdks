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

// Converts the I420 buffer backing `frame` into NV12, resizing/reusing
// `nv12_out` as needed. Returns false if the frame's buffer could not be
// obtained or converted (caller should treat this as a hard encode error
// for that frame).
bool ConvertI420ToNV12(const VideoFrame& frame, std::vector<uint8_t>* nv12_out);

// Wraps a caller-owned NV12 buffer in a newly created IMFSample suitable for
// IMFTransform::ProcessInput. On success, *out_sample is returned with a
// refcount of 1 (caller owns the reference).
HRESULT CreateNV12Sample(const uint8_t* nv12_data,
                         size_t nv12_size,
                         int width,
                         int height,
                         LONGLONG timestamp_100ns,
                         LONGLONG duration_100ns,
                         IMFSample** out_sample);

// Creates an empty output sample with a single memory buffer of at least
// `min_size` bytes, for use with IMFTransform::ProcessOutput when the MFT
// does not allocate its own output samples
// (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES not set).
HRESULT CreateEmptyOutputSample(DWORD min_size, IMFSample** out_sample);

// Maps a WebRTC H264Profile to the Media Foundation eAVEncH264VProfile enum
// used with the MF_MT_MPEG2_PROFILE output media type attribute.
eAVEncH264VProfile H264ProfileToMFProfile(H264Profile profile);

// Returns true if the H264 Annex-B bitstream in `data` contains an IDR NAL
// unit (scanning past any leading SPS/PPS/AUD NAL units). Used as a fallback
// keyframe signal when MFSampleExtension_CleanPoint isn't set by the MFT.
bool ContainsIdrNalu(const uint8_t* data, size_t size);

// Formats an HRESULT as a human-readable hex string for logging.
std::string HResultToString(HRESULT hr);

}  // namespace mf_utils
}  // namespace webrtc

#endif  // WEBRTC_MEDIA_FOUNDATION_UTILS_H_