// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <opencv2/core.hpp>
#include <opencv2/photo.hpp>
#include <stdexcept>
namespace sr {
inline cv::Mat denoise(const cv::Mat& input, int strength) {
    if(strength<0 || strength>15) throw std::runtime_error("Invalid noise reduction strength.");
    if(!strength) return input;
    cv::Mat output;
    // Filter the entire LR image before tiling; neighbouring tiles share the same pixels.
    cv::fastNlMeansDenoisingColored(input,output,float(strength),float(strength),7,21);
    return output;
}
}
