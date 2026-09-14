import { progressFeedbackFor, progressFeedbackForTool } from "./assistant-routing.js";

/**
 * Joins a finalized ASR stream to the bounded assistant and existing speaker
 * runtime. It deliberately owns no audio transport or queue.
 */
export function createSpeechSegmenter({ maxChars = 300, minChunkChars = 24, clauseChars = 72, hardChars = 140, onSegment }) {
  let buffer = "";
  let accepted = 0;
  const emit = (cut) => {
    const text = buffer.slice(0, cut).trim();
    buffer = buffer.slice(cut).trimStart();
    if (text) onSegment(text);
  };
  const drain = () => {
    while (buffer.length >= minChunkChars) {
      const sentence = buffer.match(/^([\s\S]*?[.!?])(?:\s|$)/);
      if (sentence && sentence[1].trim().length >= minChunkChars) { emit(sentence[1].length); continue; }
      if (buffer.length >= clauseChars) {
        const limit = Math.min(buffer.length, hardChars);
        const clauseMatches = [...buffer.slice(0, limit).matchAll(/[,;:](?=\s)/g)];
        const clause = clauseMatches.find((match) => match.index + 1 >= clauseChars);
        if (clause) { emit(clause.index + 1); continue; }
      }
      if (buffer.length >= hardChars) {
        const cut = buffer.slice(0, hardChars + 1).lastIndexOf(" ");
        emit(cut >= minChunkChars ? cut : hardChars);
        continue;
      }
      break;
    }
  };
  return {
    push(value) {
      if (accepted >= maxChars) return;
      const part = String(value ?? "").slice(0, maxChars - accepted);
      accepted += part.length;
      buffer += part;
      drain();
    },
    finish() { if (buffer.trim()) emit(buffer.length); },
  };
}

function createAsyncTextQueue() {
  const values = [];
  const waiters = [];
  let ended = false;
  return {
    push(value) {
      if (ended) return;
      const waiter = waiters.shift();
      if (waiter) waiter({ value, done: false });
      else values.push(value);
    },
    end() {
      ended = true;
      while (waiters.length) waiters.shift()({ value: undefined, done: true });
    },
    [Symbol.asyncIterator]() { return this; },
    next() {
      if (values.length) return Promise.resolve({ value: values.shift(), done: false });
      if (ended) return Promise.resolve({ value: undefined, done: true });
      return new Promise((resolve) => waiters.push(resolve));
    },
    return() { this.end(); return Promise.resolve({ value: undefined, done: true }); },
  };
}

export function createAssistantTurnRuntime({ assistant, speakerRuntime, isPersistentSpeakerEnabled, maxReplyChars, logger,
  progressFeedbackEnabled = false, setAssistantState = () => {} }) {
  const active = new Map();
  const generations = new Map();
  let closing = false;
  let latest = { result: "n/a", llmMs: null, llmFirstRawTokenMs: null, llmFirstTokenMs: null, llmFirstTtsChunkMs: null, llmFirstAudioMs: null, streamId: null, at: null, ttsQueuedMs: null, totalMs: null, asrFinalMs: null, progressFeedbackFired: false, progressFeedbackStartedMs: null, progressFeedbackFinishedMs: null };

  function record(result, turn, fields = {}) {
    latest = {
      result, streamId: turn?.streamId ?? null, at: Date.now(), llmMs: null, llmFirstRawTokenMs: null, llmFirstTokenMs: null,
      llmFirstTtsChunkMs: null, llmFirstAudioMs: null, ttsQueuedMs: null, totalMs: null, asrFinalMs: turn?.asrFinalMs ?? null, ...fields,
    };
  }

  function getTelemetry() {
    const assistantStatus = assistant.getTelemetry?.() ?? { enabled: true, model: null, online: "unknown", active: false };
    const hasError = ["timeout", "error", "speaker_failed", "speaker_unavailable", "unavailable"].includes(latest.result);
    return {
      ...assistantStatus,
      status: !assistantStatus.enabled ? "disabled" : active.size > 0 ? "busy" : hasError ? "error" : "ready",
      latest: { ...latest },
    };
  }

  function handleFinalTranscript(turn) {
    if (closing) return { kind: "disabled" };
    if (active.has(turn.deviceId)) {
      record("busy", turn);
      logger.warn({ device_id: turn.deviceId, stream_id: turn.streamId }, "Assistant turn ignored while device is busy");
      return { kind: "busy" };
    }
    const finalAt = performance.now();
    const generationId = (generations.get(turn.deviceId) ?? 0) + 1;
    generations.set(turn.deviceId, generationId);
    record("busy", turn);
    setAssistantState(turn.deviceId, "thinking");
    let speech = null;
    let textQueue = null;
    let progressSpeech = null;
    const routing = assistant.routeRequest?.({ text: turn.text, deviceId: turn.deviceId }) ??
      { route: "FAST", reasons: ["router_unavailable"], activity: "conversation" };
    const progressText = progressFeedbackEnabled && !routing.webTools ? progressFeedbackFor({ ...routing, variation: generationId }) : null;
    let progressFeedbackFired = false, progressFeedbackStartedAt = null, progressFeedbackFinishedAt = null;
    let progressFeedbackCancelled = false;
    let progressTimer = null;
    let toolProgressTimer = null;
    let toolProgressUsed = false;
    const completion = (async () => {
      const llmStartedAt = performance.now();
      let firstTtsChunkAt = null;
      let respondingSent = false;
      const markResponding = () => {
        if (respondingSent || generations.get(turn.deviceId) !== generationId) return;
        clearTimeout(progressTimer);
        respondingSent = true;
        setAssistantState(turn.deviceId, "responding");
      };
      if (progressText) progressTimer = setTimeout(() => {
        if (respondingSent || generations.get(turn.deviceId) !== generationId) return;
        progressFeedbackStartedAt = performance.now();
        progressSpeech = speakerRuntime.speak(progressText, { temporary: !isPersistentSpeakerEnabled(),
          replyReadyAt: progressFeedbackStartedAt, metadata: { assistant_turn: true, progress_feedback: true,
            generation_id: generationId, voice_stream_id: turn.streamId, final_at: finalAt, llm_started_at: llmStartedAt } });
        progressFeedbackFired = progressSpeech?.kind === "queued";
        if (progressFeedbackFired) progressSpeech.completion.then(() => { progressFeedbackFinishedAt = performance.now(); }).catch(() => {
          progressFeedbackFinishedAt = performance.now();
        });
      }, 800);
      progressTimer?.unref?.();
      let segmenter = null;
      const createSegmenter = (chunking = {}, profileMaxReplyChars = maxReplyChars) => createSpeechSegmenter({ maxChars: profileMaxReplyChars,
        minChunkChars: chunking.minChars, clauseChars: chunking.clauseChars, hardChars: chunking.hardChars, onSegment: (segment) => {
        if (generations.get(turn.deviceId) !== generationId) return;
        firstTtsChunkAt ??= performance.now();
        if (!textQueue) {
          textQueue = createAsyncTextQueue();
          speech = speakerRuntime.speakProgressive?.(textQueue, {
            temporary: !isPersistentSpeakerEnabled(), replyReadyAt: firstTtsChunkAt,
            metadata: { assistant_turn: true, progressive: true, voice_stream_id: turn.streamId,
              generation_id: generationId, final_at: finalAt, llm_started_at: llmStartedAt, first_tts_chunk_at: firstTtsChunkAt },
          });
          if (speech?.kind !== "queued") {
            textQueue.end();
            textQueue = null;
            speech = null;
            firstTtsChunkAt = null;
          }
        }
        if (speech?.kind === "queued") textQueue.push(segment);
      } });
      const answer = await assistant.respond({ ...turn, generationId, onFirstToken: markResponding, onSpeakableText: (text, policy) => {
        if (generations.get(turn.deviceId) !== generationId) return;
        segmenter ??= createSegmenter(policy?.chunking, policy?.maxReplyChars);
        segmenter.push(text);
      }, onToolStart: async ({ name }) => {
        clearTimeout(progressTimer);
        if (!progressFeedbackEnabled || toolProgressUsed || generations.get(turn.deviceId) !== generationId) return;
        toolProgressUsed = true;
        const phrase = progressFeedbackForTool(name, generationId);
        toolProgressTimer = setTimeout(() => {
          if (generations.get(turn.deviceId) !== generationId) return;
          progressFeedbackStartedAt = performance.now();
          progressSpeech = speakerRuntime.speak(phrase, { temporary: !isPersistentSpeakerEnabled(),
            replyReadyAt: progressFeedbackStartedAt, metadata: { assistant_turn: true, progress_feedback: true,
              web_tool: name, generation_id: generationId, voice_stream_id: turn.streamId,
              final_at: finalAt, llm_started_at: llmStartedAt } });
          progressFeedbackFired = progressSpeech?.kind === "queued";
          if (progressFeedbackFired) progressSpeech.completion.then(() => { progressFeedbackFinishedAt = performance.now(); }).catch(() => {
            progressFeedbackFinishedAt = performance.now();
          });
        }, 800);
        toolProgressTimer.unref?.();
      }, onToolEnd: async () => {
        if (toolProgressTimer) {
          clearTimeout(toolProgressTimer);
          toolProgressTimer = null;
          if (!progressFeedbackFired) progressFeedbackCancelled = true;
        }
        // Provider completion must release the next LLM round immediately.
        // The speaker queue serializes any already-started acknowledgement
        // ahead of final TTS so audio cannot overlap.
      } });
      if (generations.get(turn.deviceId) !== generationId) {
        textQueue?.end();
        speech?.cancel?.();
        return { kind: "cancelled", generationId };
      }
      if (answer.kind === "response") segmenter?.finish();
      textQueue?.end();
      const timingFields = {
        llmMs: answer.timings?.llm_request_ms ?? null,
        llmFirstRawTokenMs: answer.timings?.llm_first_raw_token_ms ?? null,
        llmFirstTokenMs: answer.timings?.llm_first_token_ms ?? null,
        llmFirstTtsChunkMs: firstTtsChunkAt == null ? null : Math.round(firstTtsChunkAt - llmStartedAt),
        reasoningRoute: answer.timings?.reasoning_route ?? routing.route,
        routingReasons: answer.timings?.routing_reasons ?? routing.reasons,
        activity: answer.timings?.activity ?? routing.activity,
        reasoningTokens: answer.timings?.reasoning_tokens ?? null,
        llmRounds: answer.timings?.llm_rounds ?? null,
        llmRoundTimings: answer.timings?.llm_round_timings ?? null,
        toolSelected: answer.timings?.tool_selected ?? [],
        toolEvents: answer.timings?.tool_events ?? [],
        agentToolsLatencyMs: answer.timings?.agent_tools_latency_ms ?? null,
        providerLatencyMs: answer.timings?.provider_latency_ms ?? null,
        structuredCapabilityUsed: answer.timings?.structured_capability_used ?? false,
        structuredTool: answer.timings?.structured_tool ?? null,
        structuredProvider: answer.timings?.structured_provider ?? null,
        structuredProviderMs: answer.timings?.structured_provider_ms ?? null,
        structuredFallbackReason: answer.timings?.structured_fallback_reason ?? null,
        capabilityPrimary: answer.timings?.capability_primary ?? null,
        capabilityRouterMs: answer.timings?.capability_router_request_ms ?? null,
        progressFeedbackFired,
        progressFeedbackCancelled,
        progressFeedbackStartedMs: progressFeedbackStartedAt == null ? null : Math.round(progressFeedbackStartedAt - llmStartedAt),
        progressFeedbackFinishedMs: progressFeedbackFinishedAt == null ? null : Math.round(progressFeedbackFinishedAt - llmStartedAt),
      };
      if (answer.kind !== "response") {
        progressSpeech?.cancel?.();
        speech?.cancel?.();
        record(answer.kind, turn, timingFields);
        logger.info({ device_id: turn.deviceId, stream_id: turn.streamId, result: answer.kind }, "Assistant turn settled without speech");
        return answer;
      }
      markResponding();
      const replyReadyAt = performance.now();
      const ttsQueuedAt = firstTtsChunkAt ?? replyReadyAt;
      const ttsQueuedMs = Math.round(ttsQueuedAt - finalAt);
      const finalToFirstTokenMs = answer.timings?.llm_first_token_ms == null ? null : Math.round(llmStartedAt - finalAt + answer.timings.llm_first_token_ms);
      speech ??= speakerRuntime.speak(answer.text, {
        // The assistant runtime already applied the active profile's reply
        // budget. Do not clip a longer profile back to the legacy turn limit.
        maxChars: Math.max(maxReplyChars ?? 0, answer.text.length),
        // Preserve the user's persistent speaker preference. The established
        // temporary receiver path is used when persistent playback is off.
        temporary: !isPersistentSpeakerEnabled(),
        replyReadyAt,
        metadata: { assistant_turn: true, progressive: false, voice_stream_id: turn.streamId, final_at: finalAt,
          generation_id: generationId, llm_started_at: llmStartedAt, first_tts_chunk_at: replyReadyAt },
      });
      if (speech.kind !== "queued") {
        record("speaker_unavailable", turn, { ...timingFields, ttsQueuedMs });
        logger.warn({ device_id: turn.deviceId, stream_id: turn.streamId, speaker_result: speech.kind }, "Assistant response was not spoken");
        return { kind: "speaker_unavailable", speaker: speech.kind };
      }
      record("playing", turn, { ...timingFields, ttsQueuedMs });
      logger.info({ device_id: turn.deviceId, stream_id: turn.streamId, playback_id: speech.playbackId,
        final_asr_to_first_llm_token_ms: finalToFirstTokenMs, final_asr_to_llm_complete_ms: Math.round(replyReadyAt - finalAt),
        final_asr_to_tts_queued_ms: ttsQueuedMs, final_to_tts_start_ms: ttsQueuedMs,
        llm_start_to_first_tts_chunk_ms: timingFields.llmFirstTtsChunkMs, progressive: Boolean(textQueue), ...answer.timings }, "Assistant TTS queued");
      try {
        const result = await speech.completion;
        const totalMs = Math.round(performance.now() - finalAt);
        record("complete", turn, { ...timingFields,
          progressFeedbackFinishedMs: progressFeedbackFinishedAt == null ? null : Math.round(progressFeedbackFinishedAt - llmStartedAt),
          llmFirstAudioMs: result?.llmStartToFirstAudioMs ?? null, ttsQueuedMs, totalMs });
        logger.info({ device_id: turn.deviceId, stream_id: turn.streamId, playback_id: speech.playbackId,
          total_final_to_playback_complete_ms: totalMs, progress_feedback_fired: progressFeedbackFired,
          progress_feedback_cancelled: progressFeedbackCancelled,
          progress_feedback_started_ms: timingFields.progressFeedbackStartedMs,
          progress_feedback_finished_ms: progressFeedbackFinishedAt == null ? null : Math.round(progressFeedbackFinishedAt - llmStartedAt) }, "Assistant turn complete");
        return { kind: "complete", result };
      } catch (error) {
        const totalMs = Math.round(performance.now() - finalAt);
        const errorCode = error?.message ?? "speaker_failed";
        record("speaker_failed", turn, { ...timingFields, ttsQueuedMs, totalMs });
        logger.warn({ device_id: turn.deviceId, stream_id: turn.streamId, playback_id: speech.playbackId,
          error_code: errorCode }, "Assistant speaker playback failed");
        return { kind: "speaker_failed", error: errorCode };
      }
    })();
    const state = { generationId, completion, cancel: () => {
      clearTimeout(progressTimer);
      clearTimeout(toolProgressTimer);
      progressSpeech?.cancel?.();
      textQueue?.end();
      speech?.cancel?.();
    } };
    active.set(turn.deviceId, state);
    completion.then((result) => {
      if (generations.get(turn.deviceId) === generationId &&
          ["timeout", "error", "speaker_failed", "speaker_unavailable", "unavailable"].includes(result?.kind))
        setAssistantState(turn.deviceId, "error");
    }).catch(() => {
      if (generations.get(turn.deviceId) === generationId) setAssistantState(turn.deviceId, "error");
    });
    completion.finally(() => {
      clearTimeout(progressTimer);
      if (active.get(turn.deviceId)?.completion === completion) active.delete(turn.deviceId);
      // Every terminal assistant path clears this state. Local speaker playback
      // independently outranks it on the ESP while it is physically active.
      if (generations.get(turn.deviceId) === generationId) setAssistantState(turn.deviceId, "idle");
    }).catch(() => {});
    return { kind: "started", completion };
  }

  function interruptDevice(deviceId, reason = "speech_start") {
    const generationId = (generations.get(deviceId) ?? 0) + 1;
    generations.set(deviceId, generationId);
    const current = active.get(deviceId);
    (assistant.cancelDevice ?? assistant.abortDevice)(deviceId);
    current?.cancel();
    if (current) active.delete(deviceId);
    setAssistantState(deviceId, "listening");
    logger.info({ device_id: deviceId, interrupted_generation_id: current?.generationId ?? null,
      generation_id: generationId, reason }, "Assistant turn interrupted");
    return generationId;
  }
  function abortDevice(deviceId) { interruptDevice(deviceId, "device_disconnect"); }
  function close() { closing = true; assistant.close(); }
  return { handleFinalTranscript, interruptDevice, abortDevice, close, getTelemetry, isActive: (deviceId) => active.has(deviceId),
    generationId: (deviceId) => generations.get(deviceId) ?? 0 };
}
