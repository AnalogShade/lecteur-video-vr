#pragma once
#include "library.h"

struct VideoFormat {
    ViewSettings view;
    bool cameraMesh = false;
    bool layoutKnown = false;
    bool projectionKnown = false;
};

VideoFormat detectFormat(const fs::path& path);
const wchar_t* projectionName(Projection mode);
const wchar_t* stereoName(Stereo mode);
