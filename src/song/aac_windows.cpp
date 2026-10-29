// SPDX-License-Identifier: MIT

// AAC on Windows: Media Foundation's AAC decoder takes the ADTS frames one at a time and gives 16-bit PCM. Its AAC
// encoder takes 16-bit PCM and gives raw AAC-LC frames, and each frame takes an ADTS header.

// clang-format off
// Windows' headers come first: Media Foundation's headers require its types.
#include <windows.h>
// clang-format on

#include "song/aac.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "song/adts.h"

namespace hokuto::song
{
namespace
{

using Microsoft::WRL::ComPtr;

// Throws the message for a call that failed, in decoding or in encoding.
void Check(HRESULT result, const char* what, const char* doing = "decode")
{
    if (FAILED(result))
    {
        std::array<char, 16> code{};
        std::snprintf(code.data(), code.size(), "0x%08lX", static_cast<unsigned long>(result));
        throw std::runtime_error(std::string("The system can't ") + doing + " the AAC stream: " + what + " failed (" +
                                 code.data() + ").");
    }
}

void CheckEncoding(HRESULT result, const char* what)
{
    Check(result, what, "encode");
}

// COM and Media Foundation, from the constructor to the destructor. A thread that started COM already keeps it.
class Session
{
public:
    Session()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com_ = SUCCEEDED(com);
        if (FAILED(com) && com != RPC_E_CHANGED_MODE)
        {
            Check(com, "starting COM");
        }
        const HRESULT started = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(started))
        {
            End();
            Check(started, "starting Media Foundation");
        }
        media_ = true;
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    ~Session()
    {
        End();
    }

private:
    void End()
    {
        if (media_)
        {
            MFShutdown();
            media_ = false;
        }
        if (com_)
        {
            CoUninitialize();
            com_ = false;
        }
    }

    bool com_ = false;
    bool media_ = false;
};

// The values of the ADTS header at the start of a stream.
struct Header
{
    uint32_t object_type = 0; // 2 for AAC-LC
    uint32_t rate_index = 0;
    uint32_t channels = 0;
};

constexpr std::array<uint32_t, 13> kRates = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                             22050, 16000, 12000, 11025, 8000,  7350};

// Media Foundation's encoder puts no priming before the samples. A decoder's first 1,024 samples come from a frame with
// no frame before it, and the samples there fade in. Silence as long as Audio Toolbox's priming goes in first. As on
// the Mac, the samples then come through whole.
constexpr std::size_t kPriming = 2112;

Header HeaderOf(std::span<const uint8_t> frame)
{
    Header header;
    header.object_type = (frame[2] >> 6) + 1u;
    header.rate_index = (frame[2] >> 2) & 0x0Fu;
    header.channels = ((frame[2] & 0x01u) << 2) | (frame[3] >> 6);
    if (header.rate_index >= kRates.size() || header.channels == 0)
    {
        throw std::runtime_error("The AAC stream's header gives no rate or no channels that Hokuto reads.");
    }

    return header;
}

// The first transform of `category` that the system offers from `input` to `output`, or the failure.
HRESULT FindTransform(const GUID& category, const MFT_REGISTER_TYPE_INFO* input, const MFT_REGISTER_TYPE_INFO* output,
                      ComPtr<IMFTransform>& transform)
{
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    const HRESULT listed =
        MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, input, output, &found, &count);
    if (FAILED(listed))
    {
        return listed;
    }
    HRESULT activated = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count; i++)
    {
        if (!transform)
        {
            activated = found[i]->ActivateObject(IID_PPV_ARGS(&transform));
        }
        found[i]->Release();
    }
    CoTaskMemFree(found);

    return activated;
}

// The decoder that the system offers for AAC.
ComPtr<IMFTransform> FindDecoder()
{
    const MFT_REGISTER_TYPE_INFO input{MFMediaType_Audio, MFAudioFormat_AAC};
    ComPtr<IMFTransform> decoder;
    Check(FindTransform(MFT_CATEGORY_AUDIO_DECODER, &input, nullptr, decoder), "starting the decoder");

    return decoder;
}

// The encoder that the system offers from PCM to AAC.
ComPtr<IMFTransform> FindEncoder()
{
    const MFT_REGISTER_TYPE_INFO input{MFMediaType_Audio, MFAudioFormat_PCM};
    const MFT_REGISTER_TYPE_INFO output{MFMediaType_Audio, MFAudioFormat_AAC};
    ComPtr<IMFTransform> encoder;
    CheckEncoding(FindTransform(MFT_CATEGORY_AUDIO_ENCODER, &input, &output, encoder), "starting the encoder");

    return encoder;
}

// The packets that the encoder has ready, appended to `bytes` as ADTS frames: one raw frame in each sample.
void TakePackets(IMFTransform& encoder, std::vector<uint8_t>& bytes, uint32_t rate, uint32_t channels)
{
    MFT_OUTPUT_STREAM_INFO info{};
    CheckEncoding(encoder.GetOutputStreamInfo(0, &info), "reading the output's size");
    const bool provides =
        (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    for (;;)
    {
        ComPtr<IMFSample> sample;
        if (!provides)
        {
            ComPtr<IMFMediaBuffer> buffer;
            CheckEncoding(MFCreateSample(&sample), "making a sample");
            CheckEncoding(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 8192), &buffer), "making a buffer");
            CheckEncoding(sample->AddBuffer(buffer.Get()), "making a sample");
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = sample.Get();
        DWORD status = 0;
        const HRESULT result = encoder.ProcessOutput(0, 1, &output, &status);
        if (output.pEvents != nullptr)
        {
            output.pEvents->Release();
        }
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            return;
        }
        CheckEncoding(result, "encoding");
        if (provides)
        {
            sample.Attach(output.pSample);
        }

        ComPtr<IMFMediaBuffer> contiguous;
        CheckEncoding(sample->ConvertToContiguousBuffer(&contiguous), "reading a frame");
        BYTE* data = nullptr;
        DWORD length = 0;
        CheckEncoding(contiguous->Lock(&data, nullptr, &length), "reading a frame");
        try
        {
            if (length > 0)
            {
                AppendAdtsFrame(bytes, std::span<const uint8_t>(data, length), rate, channels);
            }
        }
        catch (...)
        {
            contiguous->Unlock();
            throw;
        }
        contiguous->Unlock();
    }
}

// The frames that the decoder has ready, appended to `samples`.
void TakeOutput(IMFTransform& decoder, std::vector<int16_t>& samples)
{
    MFT_OUTPUT_STREAM_INFO info{};
    Check(decoder.GetOutputStreamInfo(0, &info), "reading the output's size");
    const bool provides =
        (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    for (;;)
    {
        ComPtr<IMFSample> sample;
        if (!provides)
        {
            ComPtr<IMFMediaBuffer> buffer;
            Check(MFCreateSample(&sample), "making a sample");
            Check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 65536), &buffer), "making a buffer");
            Check(sample->AddBuffer(buffer.Get()), "making a sample");
        }
        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = sample.Get();
        DWORD status = 0;
        const HRESULT result = decoder.ProcessOutput(0, 1, &output, &status);
        if (output.pEvents != nullptr)
        {
            output.pEvents->Release();
        }
        if (result == MF_E_TRANSFORM_NEED_MORE_INPUT)
        {
            return;
        }
        Check(result, "decoding");
        if (provides)
        {
            sample.Attach(output.pSample);
        }

        ComPtr<IMFMediaBuffer> contiguous;
        Check(sample->ConvertToContiguousBuffer(&contiguous), "reading the samples");
        BYTE* data = nullptr;
        DWORD length = 0;
        Check(contiguous->Lock(&data, nullptr, &length), "reading the samples");
        const std::size_t first = samples.size();
        samples.resize(first + length / sizeof(int16_t));
        std::memcpy(samples.data() + first, data, (length / sizeof(int16_t)) * sizeof(int16_t));
        contiguous->Unlock();
    }
}

} // namespace

ntdlib::wave::Pcm DecodeAdts(std::span<const uint8_t> bytes)
{
    const uint32_t frames = AdtsFrames(bytes);
    if (frames == 0)
    {
        throw std::runtime_error("The file isn't an AAC stream in ADTS.");
    }

    const Header header = HeaderOf(bytes);
    const Session session;
    const ComPtr<IMFTransform> decoder = FindDecoder();

    // The input: AAC in ADTS. The user data holds the part of HEAACWAVEINFO after its WAVEFORMATEX, with the payload's
    // type, then the AudioSpecificConfig that the header gives.
    ComPtr<IMFMediaType> input;
    Check(MFCreateMediaType(&input), "making the input's type");
    Check(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "setting the input's type");
    Check(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC), "setting the input's type");
    Check(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRates[header.rate_index]), "setting the input's rate");
    Check(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, header.channels), "setting the input's channels");
    Check(input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1), "setting the input's payload");
    const std::array<uint8_t, 14> user_data = {
        0x01,
        0x00,
        0xFE,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        static_cast<uint8_t>((header.object_type << 3) | (header.rate_index >> 1)),
        static_cast<uint8_t>(((header.rate_index & 0x01u) << 7) | (header.channels << 3))};
    Check(input->SetBlob(MF_MT_USER_DATA, user_data.data(), static_cast<UINT32>(user_data.size())),
          "setting the input's details");
    Check(decoder->SetInputType(0, input.Get(), 0), "setting the input's type");

    // The output: 16-bit PCM, among the types that the decoder offers.
    for (DWORD i = 0;; i++)
    {
        ComPtr<IMFMediaType> offered;
        Check(decoder->GetOutputAvailableType(0, i, &offered), "finding 16-bit output");
        GUID subtype{};
        UINT32 bits = 0;
        if (SUCCEEDED(offered->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == MFAudioFormat_PCM &&
            SUCCEEDED(offered->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) && bits == 16)
        {
            Check(decoder->SetOutputType(0, offered.Get(), 0), "setting the output's type");
            break;
        }
    }

    // Each frame goes in as a sample of its own, header included.
    Check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "starting the decoder");
    Check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "starting the decoder");
    std::vector<int16_t> samples;
    std::span<const uint8_t> rest = bytes;
    for (uint32_t frame = 0; frame < frames; frame++)
    {
        const std::size_t length =
            (std::size_t{rest[3] & 0x03u} << 11) | (std::size_t{rest[4]} << 3) | (std::size_t{rest[5]} >> 5);
        ComPtr<IMFMediaBuffer> buffer;
        Check(MFCreateMemoryBuffer(static_cast<DWORD>(length), &buffer), "making a buffer");
        BYTE* data = nullptr;
        Check(buffer->Lock(&data, nullptr, nullptr), "filling a buffer");
        std::memcpy(data, rest.data(), length);
        buffer->Unlock();
        Check(buffer->SetCurrentLength(static_cast<DWORD>(length)), "filling a buffer");
        ComPtr<IMFSample> sample;
        Check(MFCreateSample(&sample), "making a sample");
        Check(sample->AddBuffer(buffer.Get()), "making a sample");
        const LONGLONG start = LONGLONG{frame} * 1024 * 10'000'000 / kRates[header.rate_index];
        Check(sample->SetSampleTime(start), "making a sample");
        Check(sample->SetSampleDuration(1024LL * 10'000'000 / kRates[header.rate_index]), "making a sample");

        // A decoder that holds output takes no input until the output goes.
        HRESULT result = decoder->ProcessInput(0, sample.Get(), 0);
        if (result == MF_E_NOTACCEPTING)
        {
            TakeOutput(*decoder.Get(), samples);
            result = decoder->ProcessInput(0, sample.Get(), 0);
        }
        Check(result, "decoding");
        TakeOutput(*decoder.Get(), samples);
        rest = rest.subspan(length);
    }
    Check(decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "ending the stream");
    Check(decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "ending the stream");
    TakeOutput(*decoder.Get(), samples);

    ntdlib::wave::Pcm pcm;
    pcm.sample_rate = kRates[header.rate_index];
    pcm.channels.resize(header.channels);
    for (std::size_t i = 0; i + header.channels <= samples.size(); i += header.channels)
    {
        for (uint32_t channel = 0; channel < header.channels; channel++)
        {
            pcm.channels[channel].push_back(samples[i + channel]);
        }
    }
    pcm.loop_end = static_cast<uint32_t>(pcm.channels.front().size());

    return pcm;
}

bool EncodesConstantRate()
{
    return false;
}

std::vector<uint8_t> EncodeAdts(const ntdlib::wave::Pcm& pcm, uint32_t bit_rate)
{
    CheckEncodable(pcm, bit_rate);
    const auto channels = static_cast<uint32_t>(pcm.channels.size());
    const Session session;
    const ComPtr<IMFTransform> encoder = FindEncoder();

    // The input: 16-bit PCM. The output: raw AAC-LC at the rate of bytes that the bits give.
    ComPtr<IMFMediaType> input;
    CheckEncoding(MFCreateMediaType(&input), "making the input's type");
    CheckEncoding(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "setting the input's type");
    CheckEncoding(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM), "setting the input's type");
    CheckEncoding(input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "setting the input's samples");
    CheckEncoding(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, pcm.sample_rate), "setting the input's rate");
    CheckEncoding(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels), "setting the input's channels");
    CheckEncoding(input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * channels), "setting the input's frames");
    CheckEncoding(input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 2 * channels * pcm.sample_rate),
                  "setting the input's rate");
    ComPtr<IMFMediaType> output;
    CheckEncoding(MFCreateMediaType(&output), "making the output's type");
    CheckEncoding(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio), "setting the output's type");
    CheckEncoding(output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC), "setting the output's type");
    CheckEncoding(output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16), "setting the output's samples");
    CheckEncoding(output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, pcm.sample_rate), "setting the output's rate");
    CheckEncoding(output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels), "setting the output's channels");
    CheckEncoding(output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bit_rate / 8), "setting the output's bits");
    CheckEncoding(output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0), "setting the output's payload");
    CheckEncoding(output->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29), "setting the output's profile");

    // Some versions of the encoder require the input's type first. Others require the output's type first. The code
    // sets the input's type and then the output's. After a failure, it sets them in the other order.
    HRESULT typed = encoder->SetInputType(0, input.Get(), 0);
    if (SUCCEEDED(typed))
    {
        typed = encoder->SetOutputType(0, output.Get(), 0);
    }
    if (FAILED(typed))
    {
        CheckEncoding(encoder->SetOutputType(0, output.Get(), 0), "setting the output's type");
        CheckEncoding(encoder->SetInputType(0, input.Get(), 0), "setting the input's type");
    }

    // The priming's silence and the samples go in 1,024 frames at a time.
    CheckEncoding(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "starting the encoder");
    CheckEncoding(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "starting the encoder");
    std::vector<uint8_t> bytes;
    const std::size_t length = kPriming + pcm.channels.front().size();
    constexpr std::size_t kFrames = 1024;
    for (std::size_t first = 0; first < length; first += kFrames)
    {
        const std::size_t frames = std::min(kFrames, length - first);
        const auto size = static_cast<DWORD>(frames * channels * sizeof(int16_t));
        ComPtr<IMFMediaBuffer> buffer;
        CheckEncoding(MFCreateMemoryBuffer(size, &buffer), "making a buffer");
        BYTE* data = nullptr;
        CheckEncoding(buffer->Lock(&data, nullptr, nullptr), "filling a buffer");
        auto* samples = reinterpret_cast<int16_t*>(data);
        for (std::size_t i = 0; i < frames; i++)
        {
            const std::size_t frame = first + i;
            for (uint32_t channel = 0; channel < channels; channel++)
            {
                samples[i * channels + channel] =
                    frame < kPriming ? int16_t{0} : pcm.channels[channel][frame - kPriming];
            }
        }
        buffer->Unlock();
        CheckEncoding(buffer->SetCurrentLength(size), "filling a buffer");
        ComPtr<IMFSample> sample;
        CheckEncoding(MFCreateSample(&sample), "making a sample");
        CheckEncoding(sample->AddBuffer(buffer.Get()), "making a sample");
        CheckEncoding(sample->SetSampleTime(static_cast<LONGLONG>(first) * 10'000'000 / pcm.sample_rate),
                      "making a sample");
        CheckEncoding(sample->SetSampleDuration(static_cast<LONGLONG>(frames) * 10'000'000 / pcm.sample_rate),
                      "making a sample");

        // An encoder that holds output takes no input until the output goes.
        HRESULT result = encoder->ProcessInput(0, sample.Get(), 0);
        if (result == MF_E_NOTACCEPTING)
        {
            TakePackets(*encoder.Get(), bytes, pcm.sample_rate, channels);
            result = encoder->ProcessInput(0, sample.Get(), 0);
        }
        CheckEncoding(result, "encoding");
        TakePackets(*encoder.Get(), bytes, pcm.sample_rate, channels);
    }
    CheckEncoding(encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0), "ending the stream");
    CheckEncoding(encoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0), "ending the stream");
    TakePackets(*encoder.Get(), bytes, pcm.sample_rate, channels);

    return bytes;
}

} // namespace hokuto::song
