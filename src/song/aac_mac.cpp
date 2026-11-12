// SPDX-License-Identifier: MIT

// AAC on the Mac: Audio Toolbox reads the ADTS stream from memory, and ExtAudioFile decodes it to 16-bit PCM. An
// AudioConverter encodes 16-bit PCM as AAC-LC packets, and each packet takes an ADTS header.

#include "song/aac.h"

#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "song/adts.h"

namespace hokuto::song
{
namespace
{

// Calls `Release` on a handle of Audio Toolbox.
template <auto Release>
struct Releaser
{
    template <typename T>
    void operator()(T* handle) const
    {
        Release(handle);
    }
};

// A handle of Audio Toolbox of type `Handle`. `Release` releases the handle at the end of its scope.
template <typename Handle, auto Release>
using Owned = std::unique_ptr<std::remove_pointer_t<Handle>, Releaser<Release>>;

// Audio Toolbox reads the stream's bytes through these.
OSStatus ReadBytes(void* source, SInt64 position, UInt32 count, void* buffer, UInt32* read)
{
    const auto& bytes = *static_cast<const std::span<const uint8_t>*>(source);
    if (position < 0 || static_cast<uint64_t>(position) > bytes.size())
    {
        *read = 0;
        return kAudioFilePositionError;
    }

    const std::size_t available = bytes.size() - static_cast<std::size_t>(position);
    *read = static_cast<UInt32>(std::min<std::size_t>(count, available));
    std::memcpy(buffer, bytes.data() + position, *read);

    return noErr;
}

SInt64 SizeOf(void* source)
{
    return static_cast<SInt64>(static_cast<const std::span<const uint8_t>*>(source)->size());
}

// Throws the message for a call that failed.
void Check(OSStatus status, const char* what)
{
    if (status != noErr)
    {
        throw std::runtime_error(std::string("The system can't decode the AAC stream: ") + what + " failed (" +
                                 std::to_string(status) + ").");
    }
}

void CheckEncoding(OSStatus status, const char* what)
{
    if (status != noErr)
    {
        throw std::runtime_error(std::string("The system can't encode the AAC stream: ") + what + " failed (" +
                                 std::to_string(status) + ").");
    }
}

// The samples that the encoder takes, interleaved, and whether it has taken them.
struct EncoderInput
{
    std::vector<int16_t> samples;
    UInt32 channels = 0;
    bool given = false;
};

// Gives the encoder every sample at once, then no more.
OSStatus GiveSamples(AudioConverterRef, UInt32* packets, AudioBufferList* data, AudioStreamPacketDescription**,
                     void* user)
{
    auto& input = *static_cast<EncoderInput*>(user);
    if (input.given || input.samples.empty())
    {
        *packets = 0;
        data->mBuffers[0].mDataByteSize = 0;
        return noErr;
    }

    input.given = true;
    *packets = static_cast<UInt32>(input.samples.size() / input.channels);
    data->mNumberBuffers = 1;
    data->mBuffers[0].mNumberChannels = input.channels;
    data->mBuffers[0].mDataByteSize = static_cast<UInt32>(input.samples.size() * sizeof(int16_t));
    data->mBuffers[0].mData = input.samples.data();

    return noErr;
}

// 16-bit samples, the channels interleaved.
AudioStreamBasicDescription PcmFormat(Float64 sample_rate, UInt32 channels)
{
    AudioStreamBasicDescription samples{};
    samples.mSampleRate = sample_rate;
    samples.mFormatID = kAudioFormatLinearPCM;
    samples.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    samples.mBitsPerChannel = 16;
    samples.mChannelsPerFrame = channels;
    samples.mFramesPerPacket = 1;
    samples.mBytesPerFrame = 2 * channels;
    samples.mBytesPerPacket = samples.mBytesPerFrame;
    return samples;
}

// AAC LC.
AudioStreamBasicDescription AacFormat(Float64 sample_rate, UInt32 channels)
{
    AudioStreamBasicDescription aac{};
    aac.mSampleRate = sample_rate;
    aac.mFormatID = kAudioFormatMPEG4AAC;
    aac.mFormatFlags = kMPEG4Object_AAC_LC;
    aac.mFramesPerPacket = 1024;
    aac.mChannelsPerFrame = channels;
    return aac;
}

// The samples of the stream that `decoder` reads.
ntdlib::wave::Pcm DecodeFrames(ExtAudioFileRef decoder)
{
    ntdlib::wave::Pcm pcm;
    AudioStreamBasicDescription format{};
    UInt32 size = sizeof(format);
    Check(ExtAudioFileGetProperty(decoder, kExtAudioFileProperty_FileDataFormat, &size, &format), "reading the format");
    const UInt32 channels = format.mChannelsPerFrame;
    if (channels == 0 || format.mSampleRate <= 0.0)
    {
        throw std::runtime_error("The AAC stream has no channels or no sample rate.");
    }

    const AudioStreamBasicDescription samples = PcmFormat(format.mSampleRate, channels);
    Check(ExtAudioFileSetProperty(decoder, kExtAudioFileProperty_ClientDataFormat, sizeof(samples), &samples),
          "setting the sample format");

    pcm.sample_rate = static_cast<uint32_t>(format.mSampleRate + 0.5);
    pcm.channels.resize(channels);
    std::vector<int16_t> block(4096 * std::size_t{channels});
    for (;;)
    {
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = channels;
        list.mBuffers[0].mDataByteSize = static_cast<UInt32>(block.size() * sizeof(int16_t));
        list.mBuffers[0].mData = block.data();
        UInt32 frames = 4096;
        Check(ExtAudioFileRead(decoder, &frames, &list), "decoding");
        if (frames == 0)
        {
            break;
        }

        for (UInt32 i = 0; i < frames; i++)
        {
            for (UInt32 channel = 0; channel < channels; channel++)
            {
                pcm.channels[channel].push_back(block[std::size_t{i} * channels + channel]);
            }
        }
    }

    return pcm;
}

} // namespace

ntdlib::wave::Pcm DecodeAdts(std::span<const uint8_t> bytes)
{
    if (AdtsFrames(bytes) == 0)
    {
        throw std::runtime_error("The file isn't an AAC stream in ADTS.");
    }

    // The decoder is released before the file that it reads.
    std::span<const uint8_t> source = bytes;
    AudioFileID opened = nullptr;
    Check(AudioFileOpenWithCallbacks(&source, ReadBytes, nullptr, SizeOf, nullptr, kAudioFileAAC_ADTSType, &opened),
          "opening the stream");
    const Owned<AudioFileID, AudioFileClose> file(opened);
    ExtAudioFileRef wrapped = nullptr;
    Check(ExtAudioFileWrapAudioFileID(file.get(), false, &wrapped), "starting the decoder");
    const Owned<ExtAudioFileRef, ExtAudioFileDispose> decoder(wrapped);

    ntdlib::wave::Pcm pcm = DecodeFrames(decoder.get());
    pcm.loop_end = static_cast<uint32_t>(pcm.channels.front().size());

    return pcm;
}

bool EncodesConstantRate()
{
    return true;
}

std::vector<uint8_t> EncodeAdts(const ntdlib::wave::Pcm& pcm, uint32_t bit_rate)
{
    CheckEncodable(pcm, bit_rate);
    const auto channels = static_cast<UInt32>(pcm.channels.size());

    const AudioStreamBasicDescription samples = PcmFormat(pcm.sample_rate, channels);
    const AudioStreamBasicDescription aac = AacFormat(pcm.sample_rate, channels);

    AudioConverterRef created = nullptr;
    CheckEncoding(AudioConverterNew(&samples, &aac, &created), "starting the encoder");
    const Owned<AudioConverterRef, AudioConverterDispose> owned(created);
    AudioConverterRef converter = owned.get();
    std::vector<uint8_t> bytes;
    // A constant rate. The games' AAC files hold one. Without the mode, the encoder spends fewer bits on quiet
    // passages.
    UInt32 mode = kAudioCodecBitRateControlMode_Constant;
    CheckEncoding(AudioConverterSetProperty(converter, kAudioCodecPropertyBitRateControlMode, sizeof(mode), &mode),
                  "setting a constant bit rate");
    UInt32 rate = bit_rate;
    CheckEncoding(AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate, sizeof(rate), &rate),
                  "setting the bit rate");
    UInt32 largest = 0;
    UInt32 size = sizeof(largest);
    CheckEncoding(AudioConverterGetProperty(converter, kAudioConverterPropertyMaximumOutputPacketSize, &size, &largest),
                  "reading the packet size");

    EncoderInput input;
    input.channels = channels;
    const std::size_t length = pcm.channels.front().size();
    input.samples.reserve(length * channels);
    for (std::size_t i = 0; i < length; i++)
    {
        for (const std::vector<int16_t>& channel : pcm.channels)
        {
            input.samples.push_back(channel[i]);
        }
    }

    // A packet at a time, each with its ADTS header.
    std::vector<uint8_t> packet(std::max<UInt32>(largest, 2048));
    for (;;)
    {
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = channels;
        list.mBuffers[0].mDataByteSize = static_cast<UInt32>(packet.size());
        list.mBuffers[0].mData = packet.data();
        UInt32 packets = 1;
        AudioStreamPacketDescription description{};
        CheckEncoding(AudioConverterFillComplexBuffer(converter, GiveSamples, &input, &packets, &list, &description),
                      "encoding");
        if (packets == 0)
        {
            break;
        }

        AppendAdtsFrame(bytes, std::span(packet).first(list.mBuffers[0].mDataByteSize), pcm.sample_rate, channels);
    }

    return bytes;
}

} // namespace hokuto::song
