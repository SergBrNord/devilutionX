#include "utils/soundsample.h"

#include <chrono>
#include <cmath>
#include <utility>

#include <Aulib/DecoderDrmp3.h>
#include <Aulib/DecoderDrwav.h>
#include <Aulib/ResamplerSpeex.h>
#include <SDL.h>
#ifdef USE_SDL1
#include "utils/sdl2_to_1_2_backports.h"
#else
#include "utils/sdl2_backports.h"
#endif

#include "engine/assets.hpp"
#include "options.h"
#include "utils/log.hpp"
#include "utils/math.h"
#include "utils/stubs.h"

namespace devilution {

namespace {

constexpr float LogBase = 10.0;

/**
 * Scaling factor for attenuating volume.
 * Picked so that a volume change of -10 dB results in half perceived loudness.
 * VolumeScale = -1000 / log(0.5)
 */
constexpr float VolumeScale = 3321.9281;

/**
 * Min and max volume range, in millibel.
 * -100 dB (muted) to 0 dB (max. loudness).
 */
constexpr float MillibelMin = -10000.F;
constexpr float MillibelMax = 0.F;

/**
 * Stereo separation factor for left/right speaker panning. Lower values increase separation, moving
 * sounds further left/right, while higher values will pull sounds more towards the middle, reducing separation.
 * Current value is tuned to have ~2:1 mix for sounds that happen on the edge of a 640x480 screen.
 */
constexpr float StereoSeparation = 6000.F;

float PanLogToLinear(int logPan)
{
	if (logPan == 0)
		return 0;

	auto factor = std::pow(LogBase, static_cast<float>(-std::abs(logPan)) / StereoSeparation);

	return copysign(1.F - factor, static_cast<float>(logPan));
}

std::unique_ptr<Aulib::Decoder> CreateDecoder(bool isMp3)
{
	if (isMp3)
		return std::make_unique<Aulib::DecoderDrmp3>();
	return std::make_unique<Aulib::DecoderDrwav>();
}

std::unique_ptr<Aulib::Stream> CreateStream(SDL_RWops *handle, bool isMp3)
{
	return std::make_unique<Aulib::Stream>(handle, CreateDecoder(isMp3),
	    std::make_unique<Aulib::ResamplerSpeex>(*sgOptions.Audio.resamplingQuality), /*closeRw=*/true);
}

} // namespace

float VolumeLogToLinear(int logVolume, int logMin, int logMax)
{
	const auto logScaled = math::Remap(static_cast<float>(logMin), static_cast<float>(logMax), MillibelMin, MillibelMax, static_cast<float>(logVolume));
	return std::pow(LogBase, logScaled / VolumeScale); // linVolume
}

///// SoundSample /////

void SoundSample::Release()
{
	stream_ = nullptr;
#ifndef STREAM_ALL_AUDIO
	file_data_ = nullptr;
	file_data_size_ = 0;
#endif
}

/**
 * @brief Check if a the sound is being played atm
 */
bool SoundSample::IsPlaying()
{
	return stream_ && stream_->isPlaying();
}

/**
 * @brief Start playing the sound
 */
void SoundSample::Play(int logSoundVolume, int logUserVolume, int logPan)
{
	if (!stream_)
		return;

	const int combinedLogVolume = logSoundVolume + logUserVolume * (ATTENUATION_MIN / VOLUME_MIN);
	const float linearVolume = VolumeLogToLinear(combinedLogVolume, ATTENUATION_MIN, 0);
	stream_->setVolume(linearVolume);

	const float linearPan = PanLogToLinear(logPan);
	stream_->setStereoPosition(linearPan);

	if (!stream_->play()) {
		LogError(LogCategory::Audio, "Aulib::Stream::play (from SoundSample::Play): {}", SDL_GetError());
		return;
	}
}

/**
 * @brief Stop playing the sound
 */
void SoundSample::Stop()
{
	if (stream_)
		stream_->stop();
}

int SoundSample::SetChunkStream(std::string filePath, bool isMp3, bool logErrors)
{
	SDL_RWops *handle = OpenAsset(filePath.c_str(), /*threadsafe=*/true);
	if (handle == nullptr) {
		if (logErrors)
			LogError(LogCategory::Audio, "OpenAsset failed (from SoundSample::SetChunkStream): {}", SDL_GetError());
		return -1;
	}
	file_path_ = std::move(filePath);
	isMp3_ = isMp3;
	stream_ = CreateStream(handle, isMp3_);
	if (!stream_->open()) {
		stream_ = nullptr;
		if (logErrors)
			LogError(LogCategory::Audio, "Aulib::Stream::open (from SoundSample::SetChunkStream): {}", SDL_GetError());
		return -1;
	}
	return 0;
}

#ifndef STREAM_ALL_AUDIO
int SoundSample::SetChunk(ArraySharedPtr<std::uint8_t> fileData, std::size_t dwBytes, bool isMp3)
{
	isMp3_ = isMp3;
	file_data_ = std::move(fileData);
	file_data_size_ = dwBytes;
	SDL_RWops *buf = SDL_RWFromConstMem(file_data_.get(), dwBytes);
	if (buf == nullptr) {
		return -1;
	}

	stream_ = CreateStream(buf, isMp3_);
	if (!stream_->open()) {
		stream_ = nullptr;
		file_data_ = nullptr;
		LogError(LogCategory::Audio, "Aulib::Stream::open (from SoundSample::SetChunk): {}", SDL_GetError());
		return -1;
	}

	return 0;
}
#endif

/**
 * @return Audio duration in ms
 */
int SoundSample::GetLength() const
{
	if (!stream_)
		return 0;
	return std::chrono::duration_cast<std::chrono::milliseconds>(stream_->duration()).count();
}

} // namespace devilution
