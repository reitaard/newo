import {
  clockRequestFromSemantic,
  formatClockReply,
  parseClockRequest,
} from "./clock-request.js";

export function createClockTranscriptHandler({
  timeZone,
  sendDeviceRequest,
  speakerRuntime,
  isPersistentSpeakerEnabled,
  sendAssistantState,
  semanticResolve = null,
  logger = null,
  now = () => new Date(),
}) {
  return async function handleClockTranscript(turn) {
    const current = now();

    let request = parseClockRequest(turn.text, {
      now: current,
      timeZone,
    });

    let route = "deterministic";

    if (request.kind === "not_clock" && semanticResolve) {
      const semantic = await semanticResolve(turn.text);

      request = clockRequestFromSemantic(semantic, {
        now: current,
        timeZone,
      });

      if (request.kind !== "not_clock")
        route = "semantic";
    }

    if (request.kind === "not_clock")
      return false;

    logger?.info?.({
      device_id: turn.deviceId,
      stream_id: turn.streamId,
      clock_route: route,
      clock_action: request.action ?? request.kind,
      query_text: turn.text,
    }, "Clock transcript routed");

    sendAssistantState(turn.deviceId, "thinking");

    let reply;

    if (request.kind === "ambiguous") {
      reply = request.message;
    } else if (request.kind === "local") {
      reply = formatClockReply(
        request,
        { applied: true },
        { now: current, timeZone }
      );
    } else {
      const fields = {
        action: request.action,
      };

      if (request.epoch_s)
        fields.epoch_s = request.epoch_s;

      if (request.duration_s)
        fields.duration_s = request.duration_s;

      if (request.target)
        fields.target = request.target;

      const sent = sendDeviceRequest(
        "clock_command",
        "clock_command_ack",
        fields
      );

      if (sent.kind !== "sent") {
        reply = "The clock is unavailable right now.";
      } else {
        const outcome = await sent.promise;

        reply =
          outcome.kind === "response"
            ? formatClockReply(
                request,
                outcome.message,
                { now: current, timeZone }
              )
            : "The clock did not confirm that request.";
      }
    }

    sendAssistantState(turn.deviceId, "responding");

    const speech = speakerRuntime.speak(reply, {
      temporary: !isPersistentSpeakerEnabled(),
      metadata: {
        clock_turn: true,
        clock_route: route,
        voice_stream_id: turn.streamId,
      },
    });

    try {
      if (speech.kind === "queued")
        await speech.completion;
    } finally {
      sendAssistantState(turn.deviceId, "idle");
    }

    return true;
  };
}
