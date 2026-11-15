// Part of Engine.cpp, which #includes it in place:
// one translation unit, so this file is not compiled on its own.
void Engine::gameDetectionLoop() {
    nameCurrentThread(L"game-detection");
    {
        std::unique_lock lock(gameDetectionWaitMutex_);
        if (gameDetectionWaitCv_.wait_for(lock, std::chrono::milliseconds(900), [this] {
                return !gameDetectionRunning_.load();
            })) {
            return;
        }
    }
    if (audioCaptureWorker_) audioCaptureWorker_->startConfiguredAppSources();

    std::size_t detectionPasses = 0;
    // Foreground, GPU priority and device checks keep a fixed cadence (GPU
    // priority counts passes); an audio session change reruns only the audio
    // source pass, at once.
    constexpr auto kFullPassInterval = std::chrono::seconds(2);
    constexpr auto kMinAudioPassSpacing = std::chrono::milliseconds(100);
    auto nextFullPass = std::chrono::steady_clock::now();
    // A full pass lists the running processes once and publishes the list; an
    // audio-only pass reuses it while it still names every sounding process.
    ProcessSnapshotFeed::Snapshot processList;
    ProcessVerdictCache<GameVerdict> gameVerdicts;
    while (gameDetectionRunning_) {
        const auto passStartedAt = std::chrono::steady_clock::now();
        const bool fullPass = passStartedAt >= nextFullPass;
        const bool audioPass = (settings_.captureGameAudio || settings_.captureForegroundSystemAudio) && audioCaptureWorker_;
        ForegroundProcess foreground;
        bool foregroundIsKnownGame = false;
        if (fullPass || audioPass) {
            foreground = readForegroundProcess();
            foregroundIsKnownGame = !foreground.path.empty() && !isIgnoredForegroundProcess(foreground.exeName) &&
                isKnownGameProcess(foreground.exeName, foreground.path, foreground.processId, foreground.window);
        }
        // Read once per pass, when first needed.
        std::optional<std::vector<DWORD>> soundingIdsOfPass;
        const auto soundingIds = [&]() -> const std::vector<DWORD>& {
            if (!soundingIdsOfPass) soundingIdsOfPass = soundingAudioProcessIds();
            return *soundingIdsOfPass;
        };
        if (fullPass) {
            processList = processSnapshotFeed_.publish(RunningProcessSnapshot::captureNameOnly());
        } else if (audioPass) {
            const auto& sounding = soundingIds();
            const bool listNamesThem = processList &&
                std::all_of(sounding.begin(), sounding.end(), [&](DWORD processId) {
                    return findProcessInfo(*processList, processId) != nullptr;
                });
            if (!listNamesThem) processList = processSnapshotFeed_.publish(RunningProcessSnapshot::captureNameOnly());
        }
        if (fullPass) {
            HMONITOR activeMonitor = captureSession_ ? static_cast<HMONITOR>(captureSession_->activeMonitor()) : nullptr;
            auto [fgApp, fgIsGame] = detectForegroundAppInfo(foreground, activeMonitor);
            if (adminHotkeyRelay_) {
                adminHotkeyRelay_->observeForegroundGame(foregroundAdminGame(foreground, fgApp, fgIsGame));
            }
            if (experimentalGameWindowCaptureEnabled()) {
                const auto gameCaptureTarget =
                    detectForegroundGameCaptureTarget(foreground, foregroundIsKnownGame, activeMonitor);
                if (captureSession_ && gameCaptureTarget.window) {
                    captureSession_->preferGameWindow(gameCaptureTarget.window, gameCaptureTarget.name);
                    activeGameCaptureWindow_ = gameCaptureTarget.window;
                } else if (captureSession_ && activeGameCaptureWindow_ &&
                           (!IsWindow(static_cast<HWND>(activeGameCaptureWindow_)) ||
                            IsIconic(static_cast<HWND>(activeGameCaptureWindow_)))) {
                    captureSession_->preferMonitor();
                    activeGameCaptureWindow_ = nullptr;
                }
            } else if (captureSession_ && activeGameCaptureWindow_) {
                captureSession_->preferMonitor();
                activeGameCaptureWindow_ = nullptr;
            } else if (captureSession_ && capturePipelinePolicy().hybridGameCapture) {
                // The backend decides per frame whether the window covers the monitor.
                const auto gameCaptureTarget =
                    detectForegroundGameCaptureTarget(foreground, foregroundIsKnownGame, activeMonitor);
                if (gameCaptureTarget.window) {
                    hybridGameWindow_ = gameCaptureTarget.window;
                } else if (hybridGameWindow_ && !IsWindow(static_cast<HWND>(hybridGameWindow_))) {
                    hybridGameWindow_ = nullptr;
                }
                captureSession_->setHybridGameWindow(hybridGameWindow_);
            }

            std::lock_guard<std::mutex> lock(historyMutex_);
            auto now = std::chrono::steady_clock::now();
            if (fgApp != "Foreground app") {
                foregroundHistory_.push_back({now, fgApp, fgIsGame});
            }
            const auto cutoff = now - std::chrono::minutes(10);
            while (!foregroundHistory_.empty() && foregroundHistory_.front().timestamp < cutoff) {
                foregroundHistory_.pop_front();
            }
        }

        // The game in front and every running game, for GPU priority.
        std::string foregroundGameExe;
        std::vector<std::string> runningGameExes;
        if (audioPass) {
            // Timed from the start of the pass: the process list is part of it.
            const auto detectionStartedAt = passStartedAt;
            const RunningProcessSnapshot& processSnapshot = *processList;
            const ProcessNameSet appAudioNames(settings_.appAudioProcesses);
            const ProcessNameSet systemAudioNames(settings_.systemAudioProcesses);
            std::size_t pathQueries = foreground.processId != 0 ? 1 : 0;
            std::vector<std::string> dynamicSources;
            std::vector<std::string> detectedGames;
            std::vector<std::string> foregroundSystemApps;
            std::vector<std::string> autoApps;
            std::map<std::string, std::string> dynamicTrackAliases;
            struct GameCaptureCandidate {
                DWORD processId = 0;
                std::string exeName;
                std::string gameName;
            };
            std::vector<GameCaptureCandidate> gameCandidates;
            auto sourceProcessMatches = [](const std::string& sourceSpec, const std::string& processName) {
                const auto sourceName = lowerAscii(captureProcessName(sourceSpec));
                const auto wantedName = lowerAscii(captureProcessName(processName));
                return !sourceName.empty() && !wantedName.empty() && sourceName == wantedName;
            };
            auto forgetForegroundSystemSource = [&](const std::string& processName) {
                foregroundSystemProcesses_.erase(
                    std::remove_if(
                        foregroundSystemProcesses_.begin(),
                        foregroundSystemProcesses_.end(),
                        [&](const std::string& sourceSpec) {
                            return sourceProcessMatches(sourceSpec, processName);
                        }),
                    foregroundSystemProcesses_.end());
            };
            auto rememberForegroundSystemSource = [&](const std::string& sourceSpec) {
                if (sourceSpec.empty()) return;
                foregroundSystemProcesses_.erase(
                    std::remove_if(
                        foregroundSystemProcesses_.begin(),
                        foregroundSystemProcesses_.end(),
                        [&](const std::string& name) {
                            if (isPidProcessSpec(sourceSpec)) {
                                if (!isPidProcessSpec(name) && sourceProcessMatches(name, sourceSpec)) return true;
                                return lowerAscii(name) == lowerAscii(sourceSpec);
                            }
                            if (isPidProcessSpec(name)) {
                                return lowerAscii(name) == lowerAscii(sourceSpec);
                            }
                            return lowerAscii(captureProcessName(name)) == lowerAscii(captureProcessName(sourceSpec));
                        }),
                    foregroundSystemProcesses_.end());
                foregroundSystemProcesses_.insert(foregroundSystemProcesses_.begin(), sourceSpec);
                if (foregroundSystemProcesses_.size() > 6) {
                    foregroundSystemProcesses_.resize(6);
                }
            };
            auto rememberGameCandidate = [&](DWORD processId, const std::string& exeName) {
                if (processId == 0 || exeName.empty()) return;
                if (std::any_of(gameCandidates.begin(), gameCandidates.end(), [&](const GameCaptureCandidate& candidate) {
                        return candidate.processId == processId;
                    })) {
                    return;
                }
                forgetForegroundSystemSource(exeName);
                gameCandidates.push_back({ processId, exeName, getGameName(exeName) });
            };
            
            if (!foreground.path.empty() && !isIgnoredForegroundProcess(foreground.exeName)) {
                const DWORD fgPid = foreground.processId;
                const std::string& fgExeName = foreground.exeName;
                const bool isGame = foregroundIsKnownGame;
                if (isGame) foregroundGameExe = fgExeName;
                if (settings_.captureGameAudio && isGame &&
                    !containsProcessName(settings_.appAudioProcesses, fgExeName) &&
                    !containsProcessName(settings_.systemAudioProcesses, fgExeName) &&
                    !std::any_of(gameCandidates.begin(), gameCandidates.end(), [&](const GameCaptureCandidate& candidate) {
                        return candidate.processId == fgPid;
                    })) {
                    rememberGameCandidate(fgPid, fgExeName);
                } else if (settings_.captureForegroundSystemAudio && !isGame &&
                    !containsProcessName(settings_.appAudioProcesses, fgExeName) &&
                    !containsProcessName(settings_.systemAudioProcesses, fgExeName)) {
                    const auto foregroundSpec = appPidSourceSpec(fgPid, fgExeName);
                    const auto foregroundSource = foregroundSpec.empty() ? fgExeName : foregroundSpec;
                    rememberForegroundSystemSource(foregroundSource);
                    dynamicTrackAliases[sourceIdForProcessSpec(foregroundSource)] = "system-loopback-pcm";
                    foregroundSystemApps.push_back(fgExeName);

                    for (const auto audioProcessId : soundingIds()) {
                        if (audioProcessId == fgPid) continue;
                        const auto* audioProcess = findProcessInfo(processSnapshot, audioProcessId);
                        if (!audioProcess || audioProcess->exeName.empty()) continue;
                        if (isIgnoredForegroundProcess(audioProcess->exeName) ||
                            containsProcessName(settings_.appAudioProcesses, audioProcess->exeName) ||
                            containsProcessName(settings_.systemAudioProcesses, audioProcess->exeName)) {
                            continue;
                        }

                        const bool relatedByTree =
                            processHasAncestor(processSnapshot, audioProcessId, fgPid) ||
                            processHasAncestor(processSnapshot, fgPid, audioProcessId);
                        const bool alreadyCoveredByForegroundTree =
                            processHasAncestor(processSnapshot, audioProcessId, fgPid);
                        const bool relatedBySameExe = lowerAscii(audioProcess->exeName) == lowerAscii(fgExeName);
                        const bool alreadyCoveredByName = containsProcessName(dynamicSources, audioProcess->exeName);
                        if (alreadyCoveredByForegroundTree) continue;
                        if (alreadyCoveredByName && (!relatedBySameExe || relatedByTree)) continue;
                        const bool relatedToForeground = relatedByTree || relatedBySameExe;
                        if (!relatedToForeground) continue;

                        const auto helperSpec = appPidSourceSpec(audioProcessId, audioProcess->exeName);
                        if (helperSpec.empty()) continue;
                        rememberForegroundSystemSource(helperSpec);
                        dynamicTrackAliases[sourceIdForProcessSpec(helperSpec)] = "system-loopback-pcm";
                        foregroundSystemApps.push_back(audioProcess->exeName);
                    }
                }
            }

            if (settings_.captureGameAudio) {
                static const std::unordered_set<std::string> kSystemExes = {
                    "svchost.exe", "conhost.exe", "csrss.exe", "services.exe", "lsass.exe",
                    "dwm.exe", "smss.exe", "taskhostw.exe", "runtimebroker.exe", "searchhost.exe",
                    "audiodg.exe", "sihost.exe", "fontdrvhost.exe", "dllhost.exe", "ctfmon.exe",
                    "shellexperiencehost.exe", "startmenuexperiencehost.exe", "textinputhost.exe",
                    "systemsettings.exe", "securityhealthservice.exe", "smartscreen.exe",
                    "explorer.exe", "searchindexer.exe", "wudfhost.exe"
                };

                // A process's executable does not change while it runs, so each
                // process is asked for its path and matched against the
                // installed games once, not on every pass.
                const auto sweepStartedAt = std::chrono::steady_clock::now();
                gameVerdicts.beginPass();
                for (const auto& entry : processSnapshot.entries()) {
                    const auto& exeName = entry.exeName;
                    if (kSystemExes.contains(entry.lowerExeName) ||
                        appAudioNames.contains(entry.lowerExeName) ||
                        systemAudioNames.contains(entry.lowerExeName) ||
                        std::any_of(gameCandidates.begin(), gameCandidates.end(), [&](const GameCaptureCandidate& candidate) {
                            return candidate.processId == entry.processId;
                        })) {
                        continue;
                    }

                    bool isGame = false;
                    const auto classify = [&] {
                        ++pathQueries;
                        const auto processPath = RunningProcessSnapshot::queryProcessPath(entry.processId);
                        if (processPath.empty()) return GameVerdict::Kind::Unreadable;
                        return isKnownGameProcess(exeName, processPath, entry.processId)
                            ? GameVerdict::Kind::Game
                            : GameVerdict::Kind::NotGame;
                    };
                    if (entry.lowerExeName == "robloxplayerbeta.exe") {
                        // Roblox counts as a game only while its window is up.
                        isGame = classify() == GameVerdict::Kind::Game;
                    } else if (const auto* known =
                                   gameVerdicts.find(entry.processId, entry.parentProcessId, entry.lowerExeName);
                               known && (known->kind != GameVerdict::Kind::Unreadable || sweepStartedAt < known->retryAt)) {
                        isGame = known->kind == GameVerdict::Kind::Game;
                    } else {
                        const auto kind = classify();
                        isGame = kind == GameVerdict::Kind::Game;
                        gameVerdicts.store(entry.processId, entry.parentProcessId, entry.lowerExeName,
                            { kind, sweepStartedAt + kUnreadablePathRetry });
                    }

                    if (isGame) {
                        rememberGameCandidate(entry.processId, exeName);
                    }
                }
                gameVerdicts.endPass();

                for (const auto& candidate : gameCandidates) runningGameExes.push_back(candidate.exeName);

                const auto now = std::chrono::steady_clock::now();
                const DWORD selfProcessId = GetCurrentProcessId();
                const auto& audioSessionProcessIds = soundingIds();
                auto insideGame = [&](DWORD processId) {
                    return std::any_of(gameCandidates.begin(), gameCandidates.end(), [&](const GameCaptureCandidate& game) {
                        return processId == game.processId ||
                            processHasAncestor(processSnapshot, processId, game.processId) ||
                            processHasAncestor(processSnapshot, game.processId, processId);
                    });
                };
                // Sounding processes inside a game stay bound through quiet
                // stretches, so a silent moment does not rebind the game track.
                for (const DWORD processId : audioSessionProcessIds) {
                    const auto* info = findProcessInfo(processSnapshot, processId);
                    if (info && processId != selfProcessId && insideGame(processId)) {
                        gameAudioSessions_[processId] = { info->exeName, now };
                    }
                }
                std::erase_if(gameAudioSessions_, [&](const auto& item) {
                    const auto* info = findProcessInfo(processSnapshot, item.first);
                    return !info || lowerAscii(info->exeName) != lowerAscii(item.second.exeName) ||
                        now - item.second.lastActive > kAutoAppGrace;
                });

                std::map<std::string, std::vector<const GameCaptureCandidate*>> candidatesByGame;
                for (const auto& candidate : gameCandidates) {
                    candidatesByGame[lowerAscii(candidate.gameName)].push_back(&candidate);
                }
                for (const auto& [_, candidates] : candidatesByGame) {
                    std::vector<DWORD> candidateProcessIds;
                    candidateProcessIds.reserve(candidates.size());
                    for (const auto* candidate : candidates) candidateProcessIds.push_back(candidate->processId);

                    const auto roots = collapseProcessTreeRoots(processSnapshot.entries(), candidateProcessIds);
                    for (const DWORD rootProcessId : roots) {
                        const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const GameCaptureCandidate* candidate) {
                            return candidate->processId == rootProcessId;
                        });
                        if (found == candidates.end()) continue;

                        const auto& candidate = **found;
                        // Audio comes from the processes that play it; the game
                        // only names the track. Loopback does not reach through
                        // every launcher level (Fortnite's bootstrapper and
                        // launcher capture silence while its client plays), so
                        // the top of the game's tree is never the target.
                        std::vector<DWORD> soundingProcessIds;
                        for (const auto& session : gameAudioSessions_) {
                            const DWORD processId = session.first;
                            if (processId == rootProcessId || processHasAncestor(processSnapshot, processId, rootProcessId)) {
                                soundingProcessIds.push_back(processId);
                            }
                        }
                        for (const DWORD processId : collapseProcessTreeRoots(processSnapshot.entries(), soundingProcessIds)) {
                            const auto sourceSpec = gamePidSourceSpec(processId, gameAudioSessions_.at(processId).exeName);
                            if (sourceSpec.empty() || containsExactSourceSpec(dynamicSources, sourceSpec)) continue;
                            dynamicSources.push_back(sourceSpec);
                            dynamicTrackAliases[sourceIdForProcessSpec(sourceSpec)] = "game:" + candidate.gameName;
                        }
                        if (std::find(detectedGames.begin(), detectedGames.end(), candidate.gameName) == detectedGames.end()) {
                            detectedGames.push_back(candidate.gameName);
                        }
                    }
                }

                // Every other app with a live audio session gets its own track
                // (ADR 0028), not just the focused one: one capture per process
                // tree, none for silent apps. A session that goes quiet keeps its
                // capture for a grace period, so pausing playback does not
                // restart it.
                for (const DWORD processId : audioSessionProcessIds) {
                    const auto* info = findProcessInfo(processSnapshot, processId);
                    if (!info || processId == selfProcessId) continue;
                    const auto& exeName = info->exeName;
                    if (isIgnoredForegroundProcess(exeName) || kSystemExes.contains(lowerAscii(exeName)) ||
                        containsProcessName(settings_.appAudioProcesses, exeName)) {
                        continue;
                    }
                    if (!insideGame(processId)) autoAppSessions_[processId] = { exeName, now };
                }
                std::erase_if(autoAppSessions_, [&](const auto& item) {
                    const auto* info = findProcessInfo(processSnapshot, item.first);
                    // Gone, reused for another program, or quiet past the grace period.
                    return !info || lowerAscii(info->exeName) != lowerAscii(item.second.exeName) ||
                        now - item.second.lastActive > kAutoAppGrace;
                });
                std::vector<DWORD> autoProcessIds;
                for (const auto& [processId, _] : autoAppSessions_) autoProcessIds.push_back(processId);
                std::set<std::string> autoAppNames;
                for (const DWORD rootProcessId : collapseProcessTreeRoots(processSnapshot.entries(), autoProcessIds)) {
                    const auto& exeName = autoAppSessions_.at(rootProcessId).exeName;
                    // One track per app name; a second unrelated instance shares it.
                    if (!autoAppNames.insert(lowerAscii(exeName)).second) continue;
                    const auto sourceSpec = appPidSourceSpec(rootProcessId, exeName);
                    if (sourceSpec.empty() || containsExactSourceSpec(dynamicSources, sourceSpec)) continue;
                    dynamicSources.push_back(sourceSpec);
                    autoApps.push_back(exeName);
                }
            }

            if (settings_.captureForegroundSystemAudio) {
                foregroundSystemProcesses_.erase(
                    std::remove_if(
                        foregroundSystemProcesses_.begin(),
                        foregroundSystemProcesses_.end(),
                        [&](const std::string& name) {
                            const DWORD foregroundProcessId = isPidProcessSpec(name)
                                ? processIdFromPidSourceSpec(name)
                                : processSnapshot.processIdForName(name);
                            const bool processStillRunning = foregroundProcessId != 0 &&
                                findProcessInfo(processSnapshot, foregroundProcessId) != nullptr;
                            const bool nowCapturedAsGame = settings_.captureGameAudio &&
                                std::any_of(
                                    dynamicSources.begin(),
                                    dynamicSources.end(),
                                    [&](const std::string& sourceSpec) {
                                        const auto parsed = parseAudioProcessSpec(sourceSpec);
                                        if (parsed.kind != AudioProcessKind::Game) return false;
                                        if (sourceProcessMatches(sourceSpec, name)) return true;
                                        const DWORD gameProcessId = static_cast<DWORD>(parsed.processId);
                                        return foregroundProcessId != 0 && gameProcessId != 0 &&
                                            (processHasAncestor(processSnapshot, foregroundProcessId, gameProcessId) ||
                                             processHasAncestor(processSnapshot, gameProcessId, foregroundProcessId));
                                    });
                            return isIgnoredForegroundProcess(captureProcessName(name)) ||
                                containsProcessName(settings_.appAudioProcesses, name) ||
                                containsProcessName(settings_.systemAudioProcesses, name) ||
                                nowCapturedAsGame ||
                                !processStillRunning;
                        }),
                    foregroundSystemProcesses_.end());

                if (foregroundSystemProcesses_.size() > 6) {
                    foregroundSystemProcesses_.resize(6);
                }

                for (const auto& processName : foregroundSystemProcesses_) {
                    const bool alreadyAdded = isPidProcessSpec(processName)
                        ? containsExactSourceSpec(dynamicSources, processName)
                        : containsProcessName(dynamicSources, processName);
                    if (!alreadyAdded) {
                        dynamicSources.push_back(processName);
                        dynamicTrackAliases[sourceIdForProcessSpec(processName)] = "system-loopback-pcm";
                    }
                }
            }
            
            std::vector<std::string> mergedSources = settings_.appAudioProcesses;
            for (const auto& systemProcess : settings_.systemAudioProcesses) {
                if (!containsProcessName(mergedSources, systemProcess)) mergedSources.push_back(systemProcess);
                dynamicTrackAliases[sourceIdForProcessSpec(systemProcess)] = "system-loopback-pcm";
            }
            for (const auto& sourceSpec : dynamicSources) {
                const bool alreadyMerged = isPidProcessSpec(sourceSpec)
                    ? containsExactSourceSpec(mergedSources, sourceSpec)
                    : containsProcessName(mergedSources, sourceSpec);
                if (!alreadyMerged) {
                    mergedSources.push_back(sourceSpec);
                }
            }
            bool audioSourcesChanged = false;
            {
                std::lock_guard<std::mutex> lock(appAudioSourcesMutex_);
                audioSourcesChanged =
                    activeAppAudioSources_ != mergedSources ||
                    activeAppAudioTrackAliases_ != dynamicTrackAliases;
                activeAppAudioSources_ = mergedSources;
                activeAppAudioTrackAliases_ = dynamicTrackAliases;
            }
            if (audioSourcesChanged && !detectedGames.empty()) {
                std::cerr << "[engine] Detected games in loop:";
                for (const auto& game : detectedGames) std::cerr << " " << game;
                std::cerr << std::endl;
            }
            if (audioSourcesChanged && !autoApps.empty()) {
                std::cerr << "[engine] App tracks:";
                for (const auto& app : autoApps) std::cerr << " " << app;
                std::cerr << std::endl;
            }
            if (audioSourcesChanged && !foregroundSystemApps.empty()) {
                std::cerr << "[engine] Foreground app added to system-specific audio:";
                for (const auto& app : foregroundSystemApps) std::cerr << " " << app;
                std::cerr << std::endl;
            }
            audioCaptureWorker_->configureAppSources(mergedSources);
            refreshAudioRouting();

            ++detectionPasses;
            const auto detectionMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - detectionStartedAt).count();
            if (detectionMs >= 50 || detectionPasses % 30 == 0) {
                std::cerr << "[perf] game_detection"
                          << " ms=" << detectionMs
                          << " processes=" << processSnapshot.size()
                          << " pathQueries=" << pathQueries
                          << " dynamicSources=" << dynamicSources.size()
                          << std::endl;
            }
        }
        if (fullPass) {
            // Raises the engine's GPU class only on PCs where a game starves it.
            // The audio pass lists running games only with game audio on.
            if (!settings_.captureGameAudio || !audioCaptureWorker_) {
                detectGamesForGpuPriority(
                    foreground, foregroundIsKnownGame, *processList, foregroundGameExe, runningGameExes);
            }
            GpuPriorityControl::PipelineProgress pipeline;
            if (encoderWorker_) {
                pipeline.encoderSetups = encoderWorker_->encoderSetups();
                pipeline.framesEncoded = static_cast<uint64_t>(std::max(0, encoderWorker_->framesEncoded()));
            }
            gpuPriority_.observe(
                foregroundGameExe, runningGameExes, videoDropTotal(), monotonicNow100ns(), settings_.fps, pipeline);
            // A removed device is final for this process; the host restarts the
            // engine on exit, and a fresh process gets fresh devices. Dead GPU
            // state cannot be trusted to tear down cleanly.
            const long captureRemoved = captureSession_ ? captureSession_->deviceRemovedHr() : 0;
            const long encoderRemoved = encoderWorker_ ? encoderWorker_->deviceRemovedHr() : 0;
            if (captureRemoved != 0 || encoderRemoved != 0) {
                std::cerr << "[engine] GPU device removed (capture 0x" << std::hex << static_cast<unsigned long>(captureRemoved)
                          << ", encoder 0x" << static_cast<unsigned long>(encoderRemoved) << std::dec
                          << "); exiting so the host restarts recording." << std::endl;
                std::fflush(nullptr);
                ExitProcess(kDeviceRemovedExitCode);
            }
            nextFullPass = std::chrono::steady_clock::now() + kFullPassInterval;
        }

        std::unique_lock lock(gameDetectionWaitMutex_);
        if (!fullPass) {
            gameDetectionWaitCv_.wait_until(lock, std::min(nextFullPass, passStartedAt + kMinAudioPassSpacing), [this] {
                return !gameDetectionRunning_.load();
            });
        }
        gameDetectionWaitCv_.wait_until(lock, nextFullPass, [this] {
            return !gameDetectionRunning_.load() || audioSessionsChanged_;
        });
        audioSessionsChanged_ = false;
    }
}

