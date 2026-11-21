#pragma once

#include "video-format.h"

constexpr unsigned stereoProbeWidth = 160;
constexpr unsigned stereoProbeHeight = 120;

// Returns true when the decoded image provides enough evidence to change the automatic format.
bool inspectStereoFrame(VideoFormat& format, const std::vector<uint8_t>& luma, float packedAspect);
bool applyDecoderStereo(VideoFormat& format, const std::string& mode);
