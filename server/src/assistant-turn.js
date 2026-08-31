/**
 * Joins a finalized ASR stream to the bounded assistant and existing speaker
 * runtime. It deliberately owns no audio transport or queue.
 */
export function createAssistantTurnRuntime({ assistant, speakerRuntime, isPersistentSpeakerEnabled, maxReplyChars, logger, setAssistantState = () => {} }) {
  const active = new Map();
  let closing = false;
  let latest = { result: "n/a", llmMs: null, streamId: null, at: null, ttsQueuedMs: null, totalMs: null, asrFinalMs: null };

  function record(result, turn, fields = {}) {
    latest = {
      result, streamId: turn?.streamId ?? null, at: Date.now(), llmMs: null,
      ttsQueuedMs: null, totalMs: null, asrFinalMs: turn?.asrFinalMs ?? null, ...fields,
    };
  }

  function getTelemetry() {
    const assistantStatus = assistant.getTelemetry?.() ?? { enabled: true, model: null, qwen: "unknown", active: false };
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
    record("busy", turn);
    setAssistantState(turn.deviceId, "thinking");
    const completion = (async () => {
      const answer = await assistant.respond(turn);
      if (answer.kind !== "response") {
        record(answer.kind, turn, { llmMs: answer.timings?.llm_request_ms ?? null });
        logger.info({ device_id: turn.deviceId, stream_id: turn.streamId, result: answer.kind }, "Assistant turn settled without speech");
        return answer;
      }
      const replyReadyAt = performance.now();
      const ttsQueuedMs = Math.round(replyReadyAt - finalAt);
      const speech = speakerRuntime.speak(answer.text, {
        maxChars: maxReplyChars,
        // Preserve the user's persistent speaker preference. The established
        // temporary receiver path is used when persistent playback is off.
        temporary: !isPersistentSpeakerEnabled(),
        replyReadyAt,
        metadata: { assistant_turn: true, voice_stream_id: turn.streamId, final_at: finalAt },
      });
      if (speech.kind !== "queued") {
        record("speaker_unavailable", turn, { llmMs: answer.timings?.llm_request_ms ?? null, ttsQueuedMs });
        logger.warn({ device_id: turn.deviceId, stream_id: turn.streamId, speaker_result: speech.kind }, "Assistant response was not spoken");
        return { kind: "speaker_unavailable", speaker: speech.kind };
      }
      record("playing", turn, { llmMs: answer.timings?.llm_request_ms ?? null, ttsQueuedMs });
      logger.info({ device_id: turn.deviceId, stream_id: turn.streamId, playback_id: speech.playbackId,
        final_to_tts_start_ms: ttsQueuedMs, ...answer.timings }, "Assistant TTS queued");
      try {
        const result = await speech.completion;
        const totalMs = Math.round(performance.now() - finalAt);
        record("complete", turn, { llmMs: answer.timings?.llm_request_ms ?? null, ttsQueuedMs, totalMs });
        logger.info({ device_id: turn.deviceId, stream_id: turn.streamId, playback_id: speech.playbackId,
          total_final_to_playback_complete_ms: totalMs }, "Assistant turn complete");
        return { kind: "complete", result };
      } catch (error) {
        const totalMs = Math.round(performance.now() - finalAt);
        const errorCode = error?.message ?? "speaker_failed";
        record("speaker_failed", turn, { llmMs: answer.timings?.llm_request_ms ?? null, ttsQueuedMs, totalMs });
        logger.warn({ device_id: turn.deviceId, stream_id: turn.streamId, playback_id: speech.playbackId,
          error_code: errorCode }, "Assistant speaker playback failed");
        return { kind: "speaker_failed", error: errorCode };
      }
    })();
    active.set(turn.deviceId, completion);
    completion.finally(() => {
      if (active.get(turn.deviceId) === completion) active.delete(turn.deviceId);
      // Every terminal assistant path clears this state. Local speaker playback
      // independently outranks it on the ESP while it is physically active.
      setAssistantState(turn.deviceId, "idle");
    }).catch(() => {});
    return { kind: "started", completion };
  }

  function abortDevice(deviceId) { assistant.abortDevice(deviceId); }
  function close() { closing = true; assistant.close(); }
  return { handleFinalTranscript, abortDevice, close, getTelemetry, isActive: (deviceId) => active.has(deviceId) };
}
