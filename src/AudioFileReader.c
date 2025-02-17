/*
 * MIT License
 *
 * Copyright (c) 2018 Wudi <wudi@wudilabs.org>
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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswresample/swresample.h"
#include "Common.h"
#include "Memory.h"
#include "AudioFileCommon.h"
#include "AudioFileReader.h"

AudioFileReader *AudioFileReader_open(const TCHAR *filename, const AudioSampleType *outputSampleType) {
    int ret;
    AudioFileReader *obj = NULL;

    if ((filename == NULL) || (outputSampleType == NULL)) {
        goto err;
    }

    obj = MEMORY_ALLOC_STRUCT(AudioFileReader);
    if (obj == NULL) {
        MSG_ERROR(_T("allocating AudioFileReader struct failed\n"));
        goto err;
    }

    obj->filenameUtf8 = AudioFileCommon_getUtf8StringFromUnicodeString(filename);
    if (obj->filenameUtf8 == NULL) {
        MSG_ERROR(_T("converting filename to UTF-8 encoding failed\n"));
        goto err;
    }

    obj->outputSampleType = MEMORY_ALLOC_STRUCT(AudioSampleType);
    if (obj->outputSampleType == NULL) {
        MSG_ERROR(_T("allocating AudioSampleType struct failed\n"));
        goto err;
    }
    memcpy(obj->outputSampleType, outputSampleType, sizeof(AudioSampleType));

    // 分配一个空的 AVFormatContext
    obj->_inputFormatContext = avformat_alloc_context();
    if (obj->_inputFormatContext == NULL) {
        MSG_ERROR(_T("avformat_alloc_context() failed\n"));
        goto err;
    }

    // 打开输入文件，并读取头信息
    ret = avformat_open_input(&obj->_inputFormatContext, obj->filenameUtf8, NULL, NULL);
    if (ret < 0) {
        MSG_ERROR(_T("avformat_open_input() failed: ") _T(A_STR_FMT) _T("\n"), av_err2str(ret));
        goto err;
    }

    // 读取文件中的 packets, 获取 stream 信息
    ret = avformat_find_stream_info(obj->_inputFormatContext, NULL);
    if (ret < 0) {
        MSG_ERROR(_T("avformat_find_stream_info() failed: ") _T(A_STR_FMT) _T("\n"), av_err2str(ret));
        goto err;
    }

    // 计算总时长
    double durationInSeconds = (double)((double)obj->_inputFormatContext->duration / (double)AV_TIME_BASE);
    obj->durationInSeconds = durationInSeconds;

    if (g_verboseMode) {
        av_dump_format(obj->_inputFormatContext, 0, obj->filenameUtf8, false);
        MSG_INFO(_T("Duration: %lf seconds\n"), obj->durationInSeconds);
    }

    // 查找第一个音频流
    size_t audioStreamIndex = 0;
    while (audioStreamIndex < obj->_inputFormatContext->nb_streams) {
        if (obj->_inputFormatContext->streams[audioStreamIndex]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            break;
        }

        audioStreamIndex++;
    }
    if (audioStreamIndex == obj->_inputFormatContext->nb_streams) {
        MSG_ERROR(_T("audio stream not found\n"));
        goto err;
    }
    obj->_audioStreamIndex = audioStreamIndex;

    AVStream *stream = obj->_inputFormatContext->streams[audioStreamIndex];

    // 获取音频流的 decoder
    obj->_audioDecoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (obj->_audioDecoder == NULL) {
        MSG_ERROR(_T("audio decoder not found\n"));
        goto err;
    }

    // 创建解码器的 context
    obj->_audioDecoderContext = avcodec_alloc_context3(obj->_audioDecoder);
    if (obj->_audioDecoderContext == NULL) {
        MSG_ERROR(_T("audio decoder context alloc failed\n"));
        goto err;
    }

    // 复制音频流的 codec 参数到解码器的 context
    ret = avcodec_parameters_to_context(obj->_audioDecoderContext, stream->codecpar);
    if (ret < 0) {
        MSG_ERROR(_T("failed to copy codec parameters to decoder context\n"));
        goto err;
    }
    if (obj->_audioDecoderContext == NULL) {
        MSG_ERROR(_T("audio decoder context is null\n"));
        goto err;
    }

    // Set the packet timebase for the decoder
    // Useful for subtitles retiming by lavf (FIXME), skipping samples in audio, and video decoders such as cuvid or mediacodec.
    obj->_audioDecoderContext->pkt_timebase = stream->time_base;

    // 用找到的 decoder 初始化 codec context
    ret = avcodec_open2(obj->_audioDecoderContext, obj->_audioDecoder, NULL);
    if (ret < 0) {
        MSG_ERROR(_T("avcodec_open2() failed: ") _T(A_STR_FMT) _T("\n"), av_err2str(ret));
        goto err;
    }

    // 获取输出声道布局
    AVChannelLayout outputChannelLayout;
    if (outputSampleType->channelCount == 1) {
        outputChannelLayout = (AVChannelLayout)AV_CHANNEL_LAYOUT_MONO;
    } else if (outputSampleType->channelCount == 2) {
        outputChannelLayout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
    } else {
        MSG_ERROR(_T("wrong output channel count: %d\n"), outputSampleType->channelCount);
        goto err;
    }

    // 获取输出样本值格式
    enum AVSampleFormat outputSampleFormat = AudioFileCommon_getAvSampleFormat(obj->outputSampleType->sampleValueFormat);
    if (outputSampleFormat == AV_SAMPLE_FMT_NONE) {
        MSG_ERROR(_T("AudioFileCommon_getAvSampleFormat() failed\n"));
        goto err;
    }

    // 初始化 libswresample 重采样器的 context
    swr_alloc_set_opts2(
        &obj->_resamplerContext,                    // swresample context
        &outputChannelLayout,                       // [output] channel layout
        outputSampleFormat,                         // [output] sample format
        obj->outputSampleType->sampleRate,          // [output] sample rate
        &obj->_audioDecoderContext->ch_layout,      // [input] channel layout
        obj->_audioDecoderContext->sample_fmt,      // [input] sample format
        obj->_audioDecoderContext->sample_rate,     // [input] sample rate
        0,                                          // logging level offset
        NULL                                        // parent logging context, can be NULL
    );
    if (obj->_resamplerContext == NULL) {
        MSG_ERROR(_T("swr_alloc_set_opts2() failed\n"));
        goto err;
    }

    // 初始化 libswresample 重采样器
    ret = swr_init(obj->_resamplerContext);
    if (ret < 0) {
        MSG_ERROR(_T("swr_init() failed: ") _T(A_STR_FMT) _T("\n"), av_err2str(ret));
        goto err;
    }

    // 为输入流创建临时 packet
    obj->_tempPacket = av_packet_alloc();
    if (obj->_tempPacket == NULL) {
        MSG_ERROR(_T("av_packet_alloc() failed\n"));
        goto err;
    }

    // 为 decoder 分配临时 frame
    obj->_tempFrame = av_frame_alloc();
    if (obj->_tempFrame == NULL) {
        MSG_ERROR(_T("av_frame_alloc() failed\n"));
        goto err;
    }

    // 设置 _resamplerOutputBuffer 的初始值为空
    obj->_resamplerOutputBufferSampleCountPerChannel = 0;
    obj->_resamplerOutputBufferSize = 0;
    obj->_resamplerOutputBuffer = NULL;

    return obj;

err:
    if (obj != NULL) {
        AudioFileReader_close(&obj);
    }

    return NULL;
}

int AudioFileReader_read(AudioFileReader *obj, void *destBuffer, int destBufferSampleCountPerChannel) {
    int ret;

    // 从输入流中读取下一个 frame 为 packet
    ret = av_read_frame(obj->_inputFormatContext, obj->_tempPacket);
    if (ret < 0) {
        return -1;
    }

    // 如果 packet 不是所找到音频流的，跳过
    if (obj->_tempPacket->stream_index != obj->_audioStreamIndex) {
        return 0;
    }

    // 将该 packet 提交给解码器
    ret = avcodec_send_packet(obj->_audioDecoderContext, obj->_tempPacket);
    if (ret < 0) {
        MSG_ERROR(_T("avcodec_send_packet() failed: ") _T(A_STR_FMT) _T("\n"), av_err2str(ret));
        return -1;
    }

    // 从解码器获取可用的 frame
    ret = avcodec_receive_frame(obj->_audioDecoderContext, obj->_tempFrame);
    if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF)) {
        // those two return values are special and mean there is no output
        // frame available, but there were no errors during decoding
        return 0;
    } else if (ret < 0) {
        MSG_ERROR(_T("avcodec_receive_frame() failed: ") _T(A_STR_FMT) _T("\n"), av_err2str(ret));
        return -1;
    }

    // 获取输出样本值格式
    enum AVSampleFormat outputSampleFormat = AudioFileCommon_getAvSampleFormat(obj->outputSampleType->sampleValueFormat);

    // 分配输出 buffer 的空间
    int resamplerUpperBoundOutputSampleCountPerChannel = swr_get_out_samples(obj->_resamplerContext, obj->_tempFrame->nb_samples);
    if ((obj->_resamplerOutputBuffer == NULL)
            || (resamplerUpperBoundOutputSampleCountPerChannel > obj->_resamplerOutputBufferSampleCountPerChannel)) {
        // 如果是扩大 buffer 空间的情况，先释放原有空间
        if (obj->_resamplerOutputBuffer != NULL) {
            av_freep(&obj->_resamplerOutputBuffer);
        }

        obj->_resamplerOutputBufferSampleCountPerChannel = resamplerUpperBoundOutputSampleCountPerChannel;

        obj->_resamplerOutputBufferSize = av_samples_get_buffer_size(NULL,
                obj->outputSampleType->channelCount, resamplerUpperBoundOutputSampleCountPerChannel, outputSampleFormat, 1);

        obj->_resamplerOutputBuffer = (uint8_t*)av_malloc(obj->_resamplerOutputBufferSize);
        if (obj->_resamplerOutputBuffer == NULL) {
            MSG_ERROR(_T("allocating resampler output buffer failed\n"));
            return -1;
        }
    }

    // 转换 frame 到输出 buffer
    int outputSampleCountPerChannel = swr_convert(obj->_resamplerContext,
            &obj->_resamplerOutputBuffer, obj->_resamplerOutputBufferSampleCountPerChannel,
            (const uint8_t **)obj->_tempFrame->data, obj->_tempFrame->nb_samples);
    if (outputSampleCountPerChannel < 0) {
        MSG_ERROR(_T("swr_convert() failed\n"));
        return -1;
    }

    // 计算要复制到 destBuffer 的每声道样本数
    int copySampleCountPerChannel = outputSampleCountPerChannel;
    if (copySampleCountPerChannel > destBufferSampleCountPerChannel) {
        copySampleCountPerChannel = destBufferSampleCountPerChannel;
    }

    // 复制重采样结果到 destBuffer
    if (copySampleCountPerChannel > 0) {
        int copyDataLengthInBytes = av_samples_get_buffer_size(NULL,
                obj->outputSampleType->channelCount, copySampleCountPerChannel, outputSampleFormat, 1);
        assert(copyDataLengthInBytes <= obj->_resamplerOutputBufferSize);

        memcpy(destBuffer, obj->_resamplerOutputBuffer, copyDataLengthInBytes);
    }

    // 清扫临时 packet 的数据
    av_packet_unref(obj->_tempPacket);

    return copySampleCountPerChannel;
}

void AudioFileReader_close(AudioFileReader **objPtr) {
    if (objPtr == NULL) {
        return;
    }

    AudioFileReader *obj = *objPtr;

    if (obj->_resamplerOutputBuffer != NULL) {
        av_freep(&obj->_resamplerOutputBuffer);
    }

    if (obj->_tempFrame != NULL) {
        av_frame_free(&obj->_tempFrame);
    }

    if (obj->_tempPacket != NULL) {
        av_packet_free(&obj->_tempPacket);
    }

    if (obj->_resamplerContext != NULL) {
        swr_free(&obj->_resamplerContext);
    }

    if (obj->_audioDecoderContext != NULL) {
        avcodec_close(obj->_audioDecoderContext);
        obj->_audioDecoderContext = NULL;
    }

    if (obj->_inputFormatContext != NULL) {
        avformat_close_input(&obj->_inputFormatContext);
    }

    if (obj->outputSampleType != NULL) {
        Memory_free(&obj->outputSampleType);
    }

    if (obj->filenameUtf8 != NULL) {
        Memory_free(&obj->filenameUtf8);
    }

    Memory_free(objPtr);
}
