import { formatClockReply, parseClockRequest } from "./clock-request.js";

export function createClockTranscriptHandler({ timeZone, sendDeviceRequest, speakerRuntime,
  isPersistentSpeakerEnabled, sendAssistantState, now = () => new Date() }) {
  return async function handleClockTranscript(turn) {
    const current = now();
    const request = parseClockRequest(turn.text, { now: current, timeZone });
    if (request.kind === "not_clock") return false;
    sendAssistantState(turn.deviceId, "thinking");
    let reply;
    if (request.kind === "ambiguous") reply = request.message;
    else if (request.kind === "local") reply = formatClockReply(request, { applied: true }, { now: current, timeZone });
    else {
      const fields = { action: request.action };
      if (request.epoch_s) fields.epoch_s = request.epoch_s;
      if (request.duration_s) fields.duration_s = request.duration_s;
      if (request.target) fields.target = request.target;
      const sent = sendDeviceRequest("clock_command", "clock_command_ack", fields);
      if (sent.kind !== "sent") reply = "The clock is unavailable right now.";
      else {
        const outcome = await sent.promise;
        reply = outcome.kind === "response" ? formatClockReply(request, outcome.message, { now: current, timeZone }) :
          "The clock did not confirm that request.";
      }
    }
    sendAssistantState(turn.deviceId, "responding");
    const speech = speakerRuntime.speak(reply, { temporary: !isPersistentSpeakerEnabled(),
      metadata: { clock_turn: true, voice_stream_id: turn.streamId } });
    try { if (speech.kind === "queued") await speech.completion; }
    finally { sendAssistantState(turn.deviceId, "idle"); }
    return true;
  };
}
