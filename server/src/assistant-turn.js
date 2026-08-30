/**
 * Joins a finalized ASR stream to the bounded assistant and existing speaker
 * runtime. It deliberately owns no audio transport or queue.
 */
export function createAssistantTurnRuntime({ assistant, speakerRuntime, isPersistentSpeakerEnabled, maxReplyChars, logger }) {
  const active = new Map();
  let closing = false;

  function handleFinalTranscript(turn) {
    if (closing) return { kind: "disabled" };
    if (active.has(turn.deviceId)) {
      logger.warn({ device_id: turn.deviceId, stream_id: turn.streamId }, "Assistant turn ignored while device is busy");
      return { kind: "busy" };
    }
    const finalAt = performance.now();
    const completion = (async () => {
      const answer = await assistant.respond(turn);
      if (answer.kind !== "response") {
        logger.info({ device_id: turn.deviceId, stream_id: turn.streamId, result: answer.kind }, "Assistant turn settled without speech");
        return answer;
      }
      const replyReadyAt = performance.now();
      const speech = speakerRuntime.speak(answer.text, {
        maxChars: maxReplyChars,
        // Preserve the user's persistent speaker preference. The established
        // temporary receiver path is used when persistent playback is off.
        temporary: !isPersistentSpeakerEnabled(),
        replyReadyAt,
        metadata: { assistant_turn: true, voice_stream_id: turn.streamId, final_at: finalAt },
      });
      if (speech.kind !== "queued") {
        logger.warn({ device_id: turn.deviceId, stream_id: turn.streamId, speaker_result: speech.kind }, "Assistant response was not spoken");
        return { kind: "speaker_unavailable", speaker: speech.kind };
      }
      logger.info({ device_id: turn.deviceId, stream_id: turn.streamId, playback_id: speech.playbackId,
        final_to_tts_start_ms: Math.round(performance.now() - finalAt), ...answer.timings }, "Assistant TTS queued");
      try {
        const result = await speech.completion;
        logger.info({ device_id: turn.deviceId, stream_id: turn.streamId, playback_id: speech.playbackId,
          total_final_to_playback_complete_ms: Math.round(performance.now() - finalAt) }, "Assistant turn complete");
        return { kind: "complete", result };
      } catch (error) {
        logger.warn({ device_id: turn.deviceId, stream_id: turn.streamId, playback_id: speech.playbackId,
          error_code: error?.message ?? "speaker_failed" }, "Assistant speaker playback failed");
        return { kind: "speaker_failed", error: error?.message ?? "speaker_failed" };
      }
    })();
    active.set(turn.deviceId, completion);
    completion.finally(() => {
      if (active.get(turn.deviceId) === completion) active.delete(turn.deviceId);
    }).catch(() => {});
    return { kind: "started", completion };
  }

  function abortDevice(deviceId) { assistant.abortDevice(deviceId); }
  function close() { closing = true; assistant.close(); }
  return { handleFinalTranscript, abortDevice, close, isActive: (deviceId) => active.has(deviceId) };
}
