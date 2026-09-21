export const XIAOMEI_MODES = Object.freeze(["chat", "teach", "interpreter"]);

export function createXiaomeiSession() {
  return {
    active: false,
    mode: "chat",
    generation: 0,
    interpreter: { active: false, paused: false, sourceLanguage: "auto", targetLanguage: "auto",
      preserveTone: true, lastSource: null, lastTranslation: null, recent: [] },
    teaching: { depth: "quick", currentTopic: null, lastExample: null, lastExplanation: null,
      pronunciationTarget: null, lastPhrase: null, correctionSensitivity: "normal" },
    learner: { knownWords: [], learningWords: [], grammarTopics: [], pronunciationIssues: [],
      recentMistakes: [], recentLessons: [], practiceHistory: [] },
    lastTurn: { userText: null, assistantText: null, route: null, model: null },
  };
}

export function createXiaomeiSessionStore() {
  const sessions = new Map();
  return {
    get(deviceId) { if (!sessions.has(deviceId)) sessions.set(deviceId, createXiaomeiSession()); return sessions.get(deviceId); },
    reset(deviceId) { const next = createXiaomeiSession(); sessions.set(deviceId, next); return next; },
    delete(deviceId) { return sessions.delete(deviceId); },
    snapshot(deviceId) { return structuredClone(this.get(deviceId)); },
  };
}

export function recordInterpreterExchange(session, source, translation, direction) {
  session.interpreter.lastSource = source;
  session.interpreter.lastTranslation = translation;
  session.interpreter.recent.push({ source, translation, direction });
  if (session.interpreter.recent.length > 4) session.interpreter.recent.shift();
}
