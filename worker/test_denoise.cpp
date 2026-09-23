// SPDX-License-Identifier: GPL-3.0-or-later
#include "denoise.h"
#include <iostream>
int main() {
    cv::setNumThreads(1);
    cv::Mat noisy(64,128,CV_8UC3);
    cv::RNG random(12345);
    for(int y=0;y<noisy.rows;++y) for(int x=0;x<noisy.cols;++x) {
        auto& pixel=noisy.at<cv::Vec3b>(y,x);
        const int base=x<64?90:190;
        for(int c=0;c<3;++c) pixel[c]=cv::saturate_cast<uchar>(base+random.gaussian(7));
    }
    if(cv::norm(sr::denoise(noisy,0),noisy,cv::NORM_INF)!=0) return 1;
    const auto filtered=sr::denoise(noisy,6);
    cv::Scalar beforeMean,beforeSd,afterMean,afterSd;
    const cv::Rect flat(8,8,40,48);
    cv::meanStdDev(noisy(flat),beforeMean,beforeSd);
    cv::meanStdDev(filtered(flat),afterMean,afterSd);
    if(afterSd[0]>=beforeSd[0]*0.65 || std::abs(afterMean[0]-beforeMean[0])>3) return 2;
    if(cv::mean(filtered(cv::Rect(65,8,2,48)))[0]-cv::mean(filtered(cv::Rect(61,8,2,48)))[0]<80) return 3;
    if(sr::denoise(cv::Mat(1,1,CV_8UC3,cv::Scalar(80,90,100)),6).size()!=cv::Size(1,1)) return 4;
    try { sr::denoise(noisy,16); return 5; } catch(const std::runtime_error&) {}
    std::cout << "Noise sigma " << beforeSd[0] << " -> " << afterSd[0] << "; edge and disabled input preserved.\n";
}
