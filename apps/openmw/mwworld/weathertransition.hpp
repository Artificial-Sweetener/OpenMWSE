#ifndef OPENMW_MWWORLD_WEATHERTRANSITION_H
#define OPENMW_MWWORLD_WEATHERTRANSITION_H

/// @file Own weather and cloud-episode transitions without coupling sky renewal to precipitation changes.

#include <algorithm>
#include <cstdint>
#include <functional>

namespace MWWorld
{
    /// Keep the current, incoming, and queued weather episodes on one continuous transition clock.
    struct WeatherTransition
    {
        int mCurrentWeather = 0;
        int mNextWeather = -1;
        int mQueuedWeather = -1;
        std::uint32_t mCurrentCloudScenario = 0;
        std::uint32_t mNextCloudScenario = 0;
        std::uint32_t mQueuedCloudScenario = 0;
        /// Store the outgoing fraction; an incoming cloud with no next weather renews the current weather's sky.
        float mRemaining = 0.f;

        /// Report a coarse weather change independently of a cloud-only renewal.
        bool weatherActive() const { return mNextWeather != -1; }

        /// Report an incoming weather or cloud episode.
        bool active() const { return weatherActive() || mNextCloudScenario != 0; }

        /// Resolve the incoming episode's coarse weather without publishing a false weather change.
        int targetWeather() const { return weatherActive() ? mNextWeather : mCurrentWeather; }

        /// Replace the whole trajectory when weather is explicitly forced.
        void force(int weather, std::uint32_t scenario)
        {
            *this = {};
            mCurrentWeather = weather;
            mCurrentCloudScenario = scenario;
        }

        /// Select identities only for accepted episode boundaries and preserve an active transition when queuing
        /// weather.
        void request(int weather, bool renewEpisode, const std::function<std::uint32_t(int, int)>& selectScenario)
        {
            if (!active() && (weather != mCurrentWeather || renewEpisode))
            {
                mNextWeather = weather != mCurrentWeather ? weather : -1;
                mNextCloudScenario = selectScenario(weather, mCurrentWeather);
                mRemaining = 1.f;
            }
            else if (active() && weather != targetWeather())
            {
                mQueuedWeather = weather;
                mQueuedCloudScenario = selectScenario(weather, targetWeather());
            }
        }

        /// Advance at the incoming weather's rate, carrying excess time into the queued episode.
        void advance(float elapsedSeconds, bool fastForward, const std::function<float(int)>& transitionRate)
        {
            if (fastForward)
            {
                if (mQueuedWeather != -1)
                    force(mQueuedWeather, mQueuedCloudScenario);
                else if (active())
                    force(targetWeather(), mNextCloudScenario);
                return;
            }
            elapsedSeconds = std::max(elapsedSeconds, 0.f);
            for (int transition = 0; transition < 2 && active(); ++transition)
            {
                const float rate = transitionRate(targetWeather());
                mRemaining -= elapsedSeconds * rate;
                if (mRemaining > 0.f)
                    return;
                elapsedSeconds = rate > 0.f ? -mRemaining / rate : 0.f;
                mCurrentWeather = targetWeather();
                mCurrentCloudScenario = mNextCloudScenario;
                mNextWeather = mQueuedWeather;
                mNextCloudScenario = mQueuedCloudScenario;
                mQueuedWeather = -1;
                mQueuedCloudScenario = 0;
                mRemaining = active() ? 1.f : 0.f;
            }
        }
    };
}

#endif
