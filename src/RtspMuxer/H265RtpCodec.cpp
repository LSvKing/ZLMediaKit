﻿/*
 * MIT License
 *
 * Copyright (c) 2016 xiongziliang <771730766@qq.com>
 *
 * This file is part of ZLMediaKit(https://github.com/xiongziliang/ZLMediaKit).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "H265RtpCodec.h"

namespace mediakit{


typedef struct {
    unsigned forbidden_zero_bit :1;
    unsigned nal_ref_idc :2;
    unsigned type :5;
} NALU;

typedef struct {
    unsigned S :1;
    unsigned E :1;
    unsigned R :1;
    unsigned type :5;
} FU;

bool MakeNalu(uint8_t in, NALU &nal) {
    nal.forbidden_zero_bit = in >> 7;
    if (nal.forbidden_zero_bit) {
        return false;
    }
    nal.nal_ref_idc = (in & 0x60) >> 5;
    nal.type = in & 0x1f;
    return true;
}
bool MakeFU(uint8_t in, FU &fu) {
    fu.S = in >> 7;
    fu.E = (in >> 6) & 0x01;
    fu.R = (in >> 5) & 0x01;
    fu.type = in & 0x1f;
    if (fu.R != 0) {
        return false;
    }
    return true;
}


H265RtpDecoder::H265RtpDecoder() {
    _h265frame = obtainFrame();
}

H265Frame::Ptr  H265RtpDecoder::obtainFrame() {
    //从缓存池重新申请对象，防止覆盖已经写入环形缓存的对象
    auto frame = ResourcePoolHelper<H265Frame>::obtainObj();
    frame->buffer.clear();
    frame->iPrefixSize = 4;
    return frame;
}

bool H265RtpDecoder::inputRtp(const RtpPacket::Ptr &rtp, bool key_pos) {
    key_pos = decodeRtp(rtp);
    RtpCodec::inputRtp(rtp, key_pos);
    return key_pos;
}

bool H265RtpDecoder::decodeRtp(const RtpPacket::Ptr &rtppack) {
    /**
     * h265帧类型
     * Type==1:P/B frame
     * Type==5:IDR frame
     * Type==6:SEI frame
     * Type==7:SPS frame
     * Type==8:PPS frame
     */

    const uint8_t *frame = (uint8_t *) rtppack->payload + rtppack->offset;
    int length = rtppack->length - rtppack->offset;
    NALU nal;
    MakeNalu(*frame, nal);

    if (nal.type >= 0 && nal.type < 24) {
        //a full frame
        _h265frame->buffer.assign("\x0\x0\x0\x1", 4);
        _h265frame->buffer.append((char *)frame, length);
        _h265frame->type = nal.type;
        _h265frame->timeStamp = rtppack->timeStamp;
        _h265frame->sequence = rtppack->sequence;
        auto isIDR = _h265frame->type == 5;
        onGetH265(_h265frame);
        return (isIDR); //i frame
    }

    if (nal.type == 28) {
        //FU-A
        FU fu;
        MakeFU(frame[1], fu);
        if (fu.S == 1) {
            //FU-A start
            char tmp = (nal.forbidden_zero_bit << 7 | nal.nal_ref_idc << 5 | fu.type);
            _h265frame->buffer.assign("\x0\x0\x0\x1", 4);
            _h265frame->buffer.push_back(tmp);
            _h265frame->buffer.append((char *)frame + 2, length - 2);
            _h265frame->type = fu.type;
            _h265frame->timeStamp = rtppack->timeStamp;
            _h265frame->sequence = rtppack->sequence;
            return (_h265frame->type == 5); //i frame
        }

        if (rtppack->sequence != (uint16_t)(_h265frame->sequence + 1)) {
            _h265frame->buffer.clear();
            WarnL << "丢包,帧废弃:" << rtppack->sequence << "," << _h265frame->sequence;
            return false;
        }
        _h265frame->sequence = rtppack->sequence;
        if (fu.E == 1) {
            //FU-A end
            _h265frame->buffer.append((char *)frame + 2, length - 2);
            _h265frame->timeStamp = rtppack->timeStamp;
            auto isIDR = _h265frame->type == 5;
            onGetH265(_h265frame);
            return isIDR;
        }
        //FU-A mid
        _h265frame->buffer.append((char *)frame + 2, length - 2);
        return false;
    }

    WarnL << "不支持的rtp类型:" << nal.type << " " << rtppack->sequence;
    return false;
    // 29 FU-B     单NAL单元B模式
    // 24 STAP-A   单一时间的组合包
    // 25 STAP-B   单一时间的组合包
    // 26 MTAP16   多个时间的组合包
    // 27 MTAP24   多个时间的组合包
    // 0 udef
    // 30 udef
    // 31 udef
}

void H265RtpDecoder::onGetH265(const H265Frame::Ptr &frame) {
    //写入环形缓存
    RtpCodec::inputFrame(frame);
    _h265frame = obtainFrame();
}


////////////////////////////////////////////////////////////////////////

H265RtpEncoder::H265RtpEncoder(uint32_t ui32Ssrc,
                               uint32_t ui32MtuSize,
                               uint32_t ui32SampleRate,
                               uint8_t ui8PlayloadType,
                               uint8_t ui8Interleaved) :
        RtpInfo(ui32Ssrc,
                ui32MtuSize,
                ui32SampleRate,
                ui8PlayloadType,
                ui8Interleaved) {
}

void H265RtpEncoder::inputFrame(const Frame::Ptr &frame) {
    RtpCodec::inputFrame(frame);

    GET_CONFIG_AND_REGISTER(uint32_t,cycleMS,Rtp::kCycleMS);
    auto pcData = frame->data() + frame->prefixSize();
    auto uiStamp = frame->stamp();
    auto iLen = frame->size() - frame->prefixSize();

    uiStamp %= cycleMS;
    int iSize = _ui32MtuSize - 2;
    if (iLen > iSize) { //超过MTU
        const unsigned char s_e_r_Start = 0x80;
        const unsigned char s_e_r_Mid = 0x00;
        const unsigned char s_e_r_End = 0x40;
        //获取帧头数据，1byte
        unsigned char naluType = *((unsigned char *) pcData) & 0x1f; //获取NALU的5bit 帧类型

        unsigned char nal_ref_idc = *((unsigned char *) pcData) & 0x60; //获取NALU的2bit 帧重要程度 00 可以丢 11不能丢
        //nal_ref_idc = 0x60;
        //组装FU-A帧头数据 2byte
        unsigned char f_nri_type = nal_ref_idc + 28;//F为0 1bit,nri上面获取到2bit,28为FU-A分片类型5bit
        unsigned char s_e_r_type = naluType;
        bool bFirst = true;
        bool mark = false;
        int nOffset = 1;
        while (!mark) {
            if (iLen < nOffset + iSize) {			//是否拆分结束
                iSize = iLen - nOffset;
                mark = true;
                s_e_r_type = s_e_r_End + naluType;
            } else {
                if (bFirst == true) {
                    s_e_r_type = s_e_r_Start + naluType;
                    bFirst = false;
                } else {
                    s_e_r_type = s_e_r_Mid + naluType;
                }
            }
            memcpy(_aucSectionBuf, &f_nri_type, 1);
            memcpy(_aucSectionBuf + 1, &s_e_r_type, 1);
            memcpy(_aucSectionBuf + 2, (unsigned char *) pcData + nOffset, iSize);
            nOffset += iSize;
            makeH265Rtp(_aucSectionBuf, iSize + 2, mark, uiStamp);
        }
    } else {
        makeH265Rtp(pcData, iLen, true, uiStamp);
    }
}

void H265RtpEncoder::makeH265Rtp(const void* data, unsigned int len, bool mark, uint32_t uiStamp) {
    uint16_t ui16RtpLen = len + 12;
    uint32_t ts = htonl((_ui32SampleRate / 1000) * uiStamp);
    uint16_t sq = htons(_ui16Sequence);
    uint32_t sc = htonl(_ui32Ssrc);

    auto rtppkt = ResourcePoolHelper<RtpPacket>::obtainObj();
    unsigned char *pucRtp = rtppkt->payload;
    pucRtp[0] = '$';
    pucRtp[1] = _ui8Interleaved;
    pucRtp[2] = ui16RtpLen >> 8;
    pucRtp[3] = ui16RtpLen & 0x00FF;
    pucRtp[4] = 0x80;
    pucRtp[5] = (mark << 7) | _ui8PlayloadType;
    memcpy(&pucRtp[6], &sq, 2);
    memcpy(&pucRtp[8], &ts, 4);
    //ssrc
    memcpy(&pucRtp[12], &sc, 4);
    //playload
    memcpy(&pucRtp[16], data, len);

    rtppkt->PT = _ui8PlayloadType;
    rtppkt->interleaved = _ui8Interleaved;
    rtppkt->mark = mark;
    rtppkt->length = len + 16;
    rtppkt->sequence = _ui16Sequence;
    rtppkt->timeStamp = uiStamp;
    rtppkt->ssrc = _ui32Ssrc;
    rtppkt->type = TrackVideo;
    rtppkt->offset = 16;

    uint8_t type = H265_TYPE(((uint8_t *) (data))[0]);
    RtpCodec::inputRtp(rtppkt,type == H265Frame::isKeyFrame(type));
    _ui16Sequence++;
    _ui32TimeStamp = uiStamp;
}

}//namespace mediakit