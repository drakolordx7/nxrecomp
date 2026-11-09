// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/AudioRendererManager.cs (Ryubing fork, MIT): the session table of the audio renderer service and
// the worker thread that drives every open AudioRenderSystem through the AudioProcessor.
//
// Deviations from the C#, all forced by the port conventions or by C++ ownership:
//   * ITickSource is the struct TickSource below (steady_clock scaled to the 19.2 MHz guest tick).
//   * OpenAudioRenderer hands the renderer out as std::unique_ptr; the manager keeps raw pointers only (non-owning) in its table, as
//     the C# keeps references. The renderer unregisters itself when it is destroyed or disposed.
//   * IVirtualMemoryManager is dropped (guest memory is host memory). The work buffer is a zeroed std::vector of workBufferSize bytes.
//   * C# `Lock` (reentrant) is a std::mutex, so the C# nested `ReleaseSessionId` inside `lock (_sessionLock)` is a separate unlocked helper.
//   * C# `_isRunning` is read by the worker without the lock, so it is a std::atomic<bool>.
//   * Including this header first is fine: it includes AudioRenderSystem.h, which includes this header back at its end.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../Constants.h"
#include "../Dsp/AudioProcessor.h"
#include "../Integration/IHardwareDeviceDriver.h"
#include "../Integration/IWritableEvent.h"
#include "../audren_types.h"
#include "AudioRenderSystem.h"
#include "nxrt/log.h"

namespace nxrt::audren {

// C# ITickSource: a monotonic clock in guest ticks. Counter() is steady_clock nanoseconds scaled to 19.2 MHz: ns * 192 / 10000.
struct TickSource {
    uint64_t Counter() const {
        uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());

        return ns * 192 / 10000;
    }

    uint64_t Frequency() const { return static_cast<uint64_t>(Constants::TargetTimerFrequency); }
};

class AudioRendererManager {
  private:
    // Lock used for session allocation (C# _sessionLock). Not reentrant: the *Locked helpers below expect it to be held.
    std::mutex _sessionLock;

    std::mutex _audioProcessorLock;

    std::vector<int32_t> _sessionIds;

    std::vector<IWritableEvent*> _sessionsSystemEvent;

    std::vector<AudioRenderSystem*> _sessions;

    int32_t _activeSessionCount = 0;

    std::thread _workerThread;

    std::atomic<bool> _isRunning{false};

    IHardwareDeviceDriver* _deviceDriver = nullptr;

    std::atomic<int32_t> _disposeState{0};

  public:
    nxrt::audren::TickSource TickSource;

    AudioProcessor Processor;

    explicit AudioRendererManager(nxrt::audren::TickSource tickSource = {}) : TickSource(tickSource) {
        _sessionIds.resize(static_cast<size_t>(Constants::AudioRendererSessionCountMax));
        _sessions.assign(static_cast<size_t>(Constants::AudioRendererSessionCountMax), nullptr);
        _activeSessionCount = 0;

        for (size_t i = 0; i < _sessionIds.size(); i++) {
            _sessionIds[i] = static_cast<int32_t>(i);
        }
    }

    AudioRendererManager(const AudioRendererManager&) = delete;
    AudioRendererManager& operator=(const AudioRendererManager&) = delete;

    ~AudioRendererManager() { Dispose(); }

    void Initialize(std::vector<IWritableEvent*> sessionSystemEvents, IHardwareDeviceDriver* deviceDriver) {
        _sessionsSystemEvent = std::move(sessionSystemEvents);
        _deviceDriver = deviceDriver;
    }

    static uint64_t GetWorkBufferSize(AudioRendererConfiguration& parameter) { return AudioRenderSystem::GetWorkBufferSize(parameter); }

    void StopSendingCommands() {
        {
            std::lock_guard<std::mutex> guard(_sessionLock);

            for (AudioRenderSystem* renderer : _sessions) {
                if (renderer != nullptr) {
                    renderer->Disable();
                }
            }
        }

        {
            std::lock_guard<std::mutex> guard(_audioProcessorLock);

            if (_isRunning.load()) {
                StopLocked();
            }
        }
    }

    // Open a new AudioRenderSystem (C# OpenAudioRenderer). The renderer is returned through `renderer` (null on failure).
    // `workBufferAddress` is the guest address of the work buffer; `workBufferSize` bytes are allocated for the renderer.
    ResultCode OpenAudioRenderer(std::unique_ptr<AudioRenderSystem>& renderer, AudioRendererConfiguration& parameter, uint64_t appletResourceUserId,
                                 uint64_t workBufferAddress, uint64_t workBufferSize, uint32_t processHandle) {
        int32_t sessionId = AcquireSessionId();

        auto audioRenderer = std::make_unique<AudioRenderSystem>(*this, _sessionsSystemEvent[static_cast<size_t>(sessionId)]);

        // TODO (C#): Eventually, we should try to use the guest supplied work buffer instead of allocating our own.
        std::vector<uint8_t> workBufferMemory(static_cast<size_t>(workBufferSize), 0);

        ResultCode result = audioRenderer->Initialize(parameter, processHandle, std::move(workBufferMemory), workBufferAddress, workBufferSize,
                                                      sessionId, appletResourceUserId);

        if (result == ResultCode::Success) {
            renderer = std::move(audioRenderer);

            Register(renderer.get());
        } else {
            // The session id is released by the renderer's own Unregister when it is destroyed here (C# releases it by hand).
            audioRenderer.reset();

            renderer.reset();
        }

        return result;
    }

    void Dispose() {
        if (_disposeState.exchange(1) == 0) {
            DisposeImpl();
        }
    }

  private:
    void DisposeImpl() {
        std::vector<AudioRenderSystem*> sessions;

        {
            std::lock_guard<std::mutex> guard(_sessionLock);
            sessions = _sessions;
        }

        for (AudioRenderSystem* renderer : sessions) {
            if (renderer != nullptr) {
                renderer->Dispose();
            }
        }

        {
            std::lock_guard<std::mutex> guard(_audioProcessorLock);

            if (_isRunning.load()) {
                StopLocked();
            }
        }
    }

    int32_t AcquireSessionId() {
        std::lock_guard<std::mutex> guard(_sessionLock);

        int32_t index = _activeSessionCount;

        NXAUDREN_ASSERT(static_cast<size_t>(index) < _sessionIds.size());

        int32_t sessionId = _sessionIds[static_cast<size_t>(index)];

        _sessionIds[static_cast<size_t>(index)] = -1;

        _activeSessionCount++;

        NXLOG_I("AudioRenderer", "Registered new renderer (%d)", sessionId);

        return sessionId;
    }

    // C# ReleaseSessionId(int) without the lock: the caller holds _sessionLock.
    void ReleaseSessionIdLocked(int32_t sessionId) {
        NXAUDREN_ASSERT(_activeSessionCount > 0);

        int32_t newIndex = --_activeSessionCount;

        _sessionIds[static_cast<size_t>(newIndex)] = sessionId;
    }

    // C# HasAnyActiveRendererLocked: the caller holds _sessionLock.
    bool HasAnyActiveRendererLocked() const {
        for (AudioRenderSystem* renderer : _sessions) {
            if (renderer != nullptr) {
                return true;
            }
        }

        return false;
    }

    // C# StartLocked: the caller holds _audioProcessorLock.
    void StartLocked() {
        _isRunning.store(true);

        // TODO (C#): virtual device mapping (IAudioDevice)
        Processor.Start(_deviceDriver);

        _workerThread = std::thread([this] { SendCommandsWorker(); });
    }

    // C# StopLocked: the caller holds _audioProcessorLock.
    void StopLocked() {
        _isRunning.store(false);

        _workerThread.join();
        Processor.Stop();

        NXLOG_I("AudioRenderer", "Stopped audio renderer");
    }

    void SendCommandsWorker() {
        BoostAudioThread();
        TimerResolutionScope::Acquire();
        NXLOG_I("AudioRenderer", "Starting audio renderer");
        Processor.Wait();

        int stallLogs = 0;

        while (_isRunning.load()) {
            const auto frameStart = std::chrono::steady_clock::now();

            {
                std::lock_guard<std::mutex> guard(_sessionLock);

                for (AudioRenderSystem* renderer : _sessions) {
                    if (renderer != nullptr) {
                        renderer->SendCommands();
                    }
                }
            }

            const auto generated = std::chrono::steady_clock::now();

            Processor.Signal();
            Processor.Wait();

            // Diagnostics, not in the C#: a frame that took long to generate (waiting for the renderer's lock, which the guest's update holds)
            // or to render and pace is where a sound card would starve.
            const auto frameEnd = std::chrono::steady_clock::now();
            const double generateMs = std::chrono::duration<double, std::milli>(generated - frameStart).count();
            const double restMs = std::chrono::duration<double, std::milli>(frameEnd - generated).count();

            if ((generateMs > 15.0 || restMs > 60.0) && stallLogs < 40) {
                stallLogs++;
                NXLOG_W("AudioRenderer", "slow frame: generating the commands took %.1f ms, rendering and waiting %.1f ms", generateMs, restMs);
            }
        }

        TimerResolutionScope::Release();
    }

    void Register(AudioRenderSystem* renderer) {
        {
            std::lock_guard<std::mutex> guard(_sessionLock);

            _sessions[static_cast<size_t>(renderer->GetSessionId())] = renderer;
        }

        {
            std::lock_guard<std::mutex> guard(_audioProcessorLock);

            if (!_isRunning.load()) {
                StartLocked();
            }
        }
    }

    void Unregister(AudioRenderSystem* renderer) {
        {
            std::lock_guard<std::mutex> guard(_sessionLock);

            int32_t sessionId = renderer->GetSessionId();

            _sessions[static_cast<size_t>(renderer->GetSessionId())] = nullptr;

            // The renderer's memory is about to go away: its queued CommandList must not be processed after that.
            Processor.DiscardSession(sessionId);

            ReleaseSessionIdLocked(sessionId);

            NXLOG_I("AudioRenderer", "Unregistered renderer (%d)", sessionId);
        }

        {
            std::lock_guard<std::mutex> guard(_audioProcessorLock);

            if (_isRunning.load() && !HasAnyActiveRendererLocked()) {
                StopLocked();
            }
        }
    }

    friend class AudioRenderSystem;
};

inline uint64_t AudioRenderSystem::GetSystemTicks() { return _manager.TickSource.Counter(); }

inline void AudioRenderSystem::SendCommands() {
    std::lock_guard<std::mutex> guard(_lock);

    if (_isActive) {
        if (!_manager.Processor.HasRemainingCommands(_sessionId)) {
            std::unique_ptr<CommandList> commands = GenerateCommandList();

            _manager.Processor.Send(_sessionId, std::move(commands), GetMaxAllocatedTimeForDsp(), _appletResourceId);

            if (_systemEvent != nullptr) {
                _systemEvent->Signal();
            }
        } else {
            _isDspRunningBehind = true;
        }
    }
}

inline RendererSystemContext AudioRenderSystem::GetContext() {
    RendererSystemContext context;

    context.ChannelCount = _manager.Processor.OutputDevices[static_cast<size_t>(_sessionId)]->GetChannelCount();
    context.BehaviourInfo = &_behaviourInfo;
    context.DepopBuffer = _depopBuffer;
    context.MixBufferCount = GetMixBufferCount();
    context.SessionId = _sessionId;
    context.UpsamplerManager = _upsamplerManager.get();

    return context;
}

inline void AudioRenderSystem::DisposeImpl() {
    if (_isActive) {
        Stop();
    }

    PoolMapper mapper(_processHandle, false);
    mapper.Unmap(_dspMemoryPoolInfo);

    PoolMapper::ClearUsageState(_memoryPools);

    for (MemoryPoolInfo& memoryPool : _memoryPools) {
        if (memoryPool.IsMapped()) {
            mapper.Unmap(memoryPool);
        }
    }

    _manager.Unregister(this);

}

inline AudioRenderSystem::~AudioRenderSystem() {
    Dispose();
    g_gauge_voices_in_use.fetch_sub(_gaugeVoices, std::memory_order_relaxed);
    g_gauge_wavebuffers_queued.fetch_sub(_gaugeWaves, std::memory_order_relaxed);
    g_gauge_memory_pools.fetch_sub(_gaugePools, std::memory_order_relaxed);
}

}
