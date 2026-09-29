// SPDX-License-Identifier: GPL-3.0-or-later
#include "color_pipeline.h"
#include <QColorSpace>
#include <QFile>
#include <QtEndian>
#include <lcms2.h>
#include <zlib.h>
#include <array>
#include <memory>
#include <vector>

namespace Sr {
namespace {
constexpr int profileLimit = 16 * 1024 * 1024;
using CmsProfile = std::unique_ptr<void, decltype(&cmsCloseProfile)>;
CmsProfile open(const QByteArray& bytes) {
    return CmsProfile(cmsOpenProfileFromMem(bytes.constData(),cmsUInt32Number(bytes.size())),cmsCloseProfile);
}
QByteArray extractJpeg(QFile& file, QString& error, bool& cmyk) {
    std::array<QByteArray,256> parts;
    std::array<bool,256> seen{};
    int count=0;
    file.seek(2);
    while (!file.atEnd() && file.pos()<64*1024*1024) {
        char c=0; if(!file.getChar(&c) || uchar(c)!=0xff) break;
        do { if(!file.getChar(&c)) return {}; } while(uchar(c)==0xff);
        const uchar marker=uchar(c);
        if(marker==0xda || marker==0xd9) break;
        if(marker==0x01 || (marker>=0xd0 && marker<=0xd7)) continue;
        const auto lengthBytes=file.read(2);
        if(lengthBytes.size()!=2) break;
        const int length=qFromBigEndian<quint16>(lengthBytes.constData())-2;
        if(length<0) break;
        const auto data=file.read(length);
        if(data.size()!=length) break;
        if((marker>=0xc0 && marker<=0xcf) && marker!=0xc4 && marker!=0xc8 && marker!=0xcc && data.size()>5)
            cmyk=uchar(data[5])==4;
        if(marker==0xe2 && data.startsWith(QByteArray("ICC_PROFILE\0",12))) {
            if(data.size()<14) { error=QStringLiteral("JPEG ICCヘッダーが破損しています"); return {}; }
            int sequence=uchar(data[12]), total=uchar(data[13]);
            if(!sequence || !total || sequence>total || (count && count!=total) || seen[sequence]) {
                error=QStringLiteral("JPEG ICCの分割情報が不正です"); return {};
            }
            count=total; seen[sequence]=true; parts[sequence]=data.mid(14);
        }
    }
    QByteArray icc;
    for(int i=1;i<=count;++i) {
        if(!seen[i]) { error=QStringLiteral("JPEG ICCの一部が欠けています"); return {}; }
        icc+=parts[i];
    }
    if(icc.size()>profileLimit) { error=QStringLiteral("ICCが大きすぎます"); return {}; }
    return icc;
}
QByteArray extractWebp(QFile& file, QString& error) {
    file.seek(12);
    while(!file.atEnd()) {
        const auto header=file.read(8);
        if(header.size()!=8) break;
        const quint32 size=qFromLittleEndian<quint32>(header.constData()+4);
        if(qint64(size)>file.size()-file.pos()) { error=QStringLiteral("WebPチャンクが破損しています"); return {}; }
        if(header.left(4)=="ICCP") {
            if(size>profileLimit) { error=QStringLiteral("WebP ICCが大きすぎます"); return {}; }
            return file.read(size);
        }
        if(!file.seek(file.pos()+size+(size&1))) break;
    }
    return {};
}
QByteArray extractPng(QFile& file, QString& error) {
    file.seek(8);
    while(!file.atEnd() && file.pos()<64*1024*1024) {
        const auto header=file.read(8);
        if(header.size()!=8) break;
        const quint32 length=qFromBigEndian<quint32>(header.constData());
        const auto type=header.mid(4);
        if(type=="IDAT" || type=="IEND") break;
        if(type!="iCCP") { if(!file.seek(file.pos()+qint64(length)+4)) break; continue; }
        if(length>profileLimit) { error=QStringLiteral("PNG ICCが大きすぎます"); return {}; }
        const auto data=file.read(length), checksum=file.read(4);
        if(data.size()!=length || checksum.size()!=4) { error=QStringLiteral("PNG ICCが破損しています"); return {}; }
        uLong crc=crc32(0,reinterpret_cast<const Bytef*>(type.constData()),4);
        crc=crc32(crc,reinterpret_cast<const Bytef*>(data.constData()),uInt(data.size()));
        const int separator=data.indexOf('\0');
        if(crc!=qFromBigEndian<quint32>(checksum.constData()) || separator<1 || separator>79 ||
           separator+2>=data.size() || data[separator+1]!=0) { error=QStringLiteral("PNG ICCの形式が不正です"); return {}; }
        QByteArray result(profileLimit,0); uLongf size=profileLimit;
        const int status=uncompress(reinterpret_cast<Bytef*>(result.data()),&size,
            reinterpret_cast<const Bytef*>(data.constData()+separator+2),uLong(data.size()-separator-2));
        if(status!=Z_OK) { error=QStringLiteral("PNG ICCを展開できません"); return {}; }
        result.resize(int(size)); return result;
    }
    return {};
}
}

QByteArray srgbProfile() { return QColorSpace(QColorSpace::SRgb).iccProfile(); }

Profile readProfile(const QString& path,const QImage& decoded) {
    Profile result;
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) { result.error=QStringLiteral("画像のICCを読み込めません"); return result; }
    const auto magic=file.peek(12);
    bool cmyk=false;
    if(magic.startsWith(QByteArray::fromHex("ffd8"))) result.icc=extractJpeg(file,result.error,cmyk);
    else if(magic.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) result.icc=extractPng(file,result.error);
    else if(magic.startsWith("RIFF") && magic.mid(8,4)=="WEBP") result.icc=extractWebp(file,result.error);
    // Other formats use the decoder's RGB pixels and colour-space metadata.
    // Qt has already converted CMYK JPEG pixels to RGB; the original CMYK ICC
    // cannot be applied to those RGB values a second time.
    if(cmyk) {
        result.icc=srgbProfile(); result.error.clear(); result.assumedSrgb=true;
        result.description=QStringLiteral("CMYK画像のデコーダーRGB出力 → sRGBと仮定");
    }
    if(!result.error.isEmpty()) return result;
    if(result.icc.isEmpty() && !decoded.colorSpace().iccProfile().isEmpty()) result.icc=decoded.colorSpace().iccProfile();
    if(result.icc.isEmpty()) {
        result.icc=srgbProfile(); result.assumedSrgb=true;
        result.description=QStringLiteral("ICCなし → sRGBと仮定");
    }
    auto profile=open(result.icc);
    if(!profile) { result.error=QStringLiteral("埋め込みICCが破損または未対応です"); return result; }
    const auto color=cmsGetColorSpace(profile.get());
    if(color!=cmsSigRgbData && color!=cmsSigGrayData) { result.error=QStringLiteral("RGB/Gray以外のICCは未対応です"); return result; }
    if(!result.assumedSrgb) {
        char name[512]{};
        cmsGetProfileInfoASCII(profile.get(),cmsInfoDescription,"en","US",name,sizeof name);
        result.description=QString::fromUtf8(name);
        if(result.description.isEmpty()) result.description=QStringLiteral("埋め込みICC");
    }
    if(decoded.depth()>32) result.description+=QStringLiteral(" · SRは8-bitへ変換");
    return result;
}

QImage convert(const QImage& source,const Profile& profile,const QByteArray& destination,QString* error) {
    auto fail=[&](const QString& message) { if(error) *error=message; return QImage(); };
    if(source.isNull()) return fail(QStringLiteral("画像がありません"));
    if(!profile.error.isEmpty()) return fail(profile.error);
    auto input=open(profile.icc), output=open(destination.isEmpty()?srgbProfile():destination);
    if(!input || !output || cmsGetColorSpace(output.get())!=cmsSigRgbData)
        return fail(QStringLiteral("表示先または入力ICCを読み込めません"));
    QImage straight=source.convertToFormat(QImage::Format_RGBA8888);
    QImage converted(straight.size(),QImage::Format_RGBA8888);
    if(straight.isNull() || converted.isNull()) return fail(QStringLiteral("色変換用メモリーを確保できません"));
    const bool gray=cmsGetColorSpace(input.get())==cmsSigGrayData;
    cmsHTRANSFORM transform=cmsCreateTransform(input.get(),gray?TYPE_GRAY_8:TYPE_RGBA_8,
        output.get(),gray?TYPE_RGB_8:TYPE_RGBA_8,INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_BLACKPOINTCOMPENSATION | (gray?0:cmsFLAGS_COPY_ALPHA));
    if(!transform) return fail(QStringLiteral("ICC間の色変換を作成できません"));
    std::vector<uchar> grays(size_t(straight.width())),rgb(size_t(straight.width())*3);
    for(int y=0;y<straight.height();++y) {
        const uchar* src=straight.constScanLine(y); uchar* dst=converted.scanLine(y);
        if(gray) {
            for(int x=0;x<straight.width();++x) grays[size_t(x)]=src[x*4];
            cmsDoTransform(transform,grays.data(),rgb.data(),cmsUInt32Number(straight.width()));
            for(int x=0;x<straight.width();++x) {
                for(int c=0;c<3;++c) dst[x*4+c]=rgb[size_t(x)*3+c];
                dst[x*4+3]=src[x*4+3];
            }
        } else cmsDoTransform(transform,src,dst,cmsUInt32Number(straight.width()));
    }
    cmsDeleteTransform(transform);
    converted.setColorSpace(QColorSpace::fromIccProfile(destination.isEmpty()?srgbProfile():destination));
    return converted;
}
QImage toSrgb(const QImage& source,const Profile& profile,QString* error) {
    auto result=convert(source,profile,srgbProfile(),error);
    if(!result.isNull()) result.setColorSpace(QColorSpace::SRgb);
    return result;
}
}
