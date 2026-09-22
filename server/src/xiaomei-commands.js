const normalize = (text) => String(text ?? "").trim().toLowerCase().replace(/[.!?。！？]+$/u, "").replace(/\s+/g, " ");

const COMMANDS = Object.freeze([
  { id: "session.activate", modes: ["inactive", "chat", "teach", "interpreter"], match: /^(?:玲玲|ling\s*ling|xiaomei|小美)$/iu },
  { id: "session.exit", modes: ["chat", "teach", "interpreter"], match: /^(?:exit|leave|stop) xiaomei$|^(?:再见|退出小美)$/iu },
  { id: "mode.chat", modes: ["chat", "teach", "interpreter"], match: /^(?:chat mode|chat with me|go back to (?:normal )?chat|talk to me)$/i },
  { id: "mode.teach", modes: ["chat", "teach", "interpreter"], match: /^(?:teaching mode|teach me chinese|practice (?:chinese )?with me)$/i },
  { id: "interpreter.start", modes: ["chat", "teach"], match: /^(?:start (?:translating|translation|interpreter)(?: between us)?|translate between us|interpreter mode)$/i },
  { id: "interpreter.stop", modes: ["interpreter"], match: /^(?:stop (?:translating|translation|interpreter)|leave interpreter)$/i },
  { id: "interpreter.pause", modes: ["interpreter"], match: /^(?:pause|pause (?:translating|translation|interpreter))$/i },
  { id: "interpreter.resume", modes: ["interpreter"], match: /^(?:resume|continue|resume (?:translating|translation|interpreter))$/i },
  { id: "interpreter.auto", modes: ["interpreter"], match: /^(?:automatic languages?|detect languages? automatically)$/i },
  { id: "interpreter.en_zh", modes: ["interpreter"], match: /^(?:english to chinese|translate english to chinese)$/i },
  { id: "interpreter.zh_en", modes: ["interpreter"], match: /^(?:chinese to english|translate chinese to english)$/i },
  { id: "interpreter.switch", modes: ["interpreter"], match: /^(?:switch languages?|reverse languages?)$/i },
  { id: "interpreter.preserve", modes: ["interpreter"], match: /^(?:preserve (?:the )?(?:tone|register|exactly what they say)|keep the tone)$/i },
  { id: "interpreter.literal", modes: ["interpreter"], match: /^(?:translate literally|literal translation)$/i },
  { id: "interpreter.natural", modes: ["interpreter"], match: /^(?:translate naturally|natural translation)$/i },
  { id: "interpreter.meta", modes: ["interpreter"], match: /^(?:what did (?:she|he|they) (?:say|mean)|what does .+ mean|explain (?:that|what .+ said)|don't translate this[,;:]?.*|why did .+ use .+|was (?:she|he|that) .+|does (?:she|he|that) mean .+)$/i },
  { id: "speech.repeat", modes: ["chat", "teach", "interpreter"], match: /^(?:say|repeat) that again$|^repeat$/i },
  { id: "speech.slower", modes: ["chat", "teach", "interpreter"], match: /^(?:say (?:it|that) )?(much )?slower$/i },
  { id: "speech.normal", modes: ["chat", "teach", "interpreter"], match: /^(?:speak normally|normal speed)$/i },
  { id: "teach.more", modes: ["teach", "chat"], match: /^(?:explain more|break (?:that|it) down|give me an example|another example)$/i },
  { id: "teach.pinyin", modes: ["teach", "chat"], match: /^(?:give me the pinyin|what tone is that|tones?|pinyin)$/i },
  { id: "teach.practice", modes: ["teach", "chat"], match: /^(?:correct me|was that right|quiz me|test me|practice with me)$/i },
  { id: "teach.chinese_only", modes: ["teach", "chat"], match: /^(?:say only the chinese|chinese only)$/i },
]);

export function resolveXiaomeiCommand(text, session) {
  const value = normalize(text);
  const mode = session.active ? session.mode : "inactive";
  const command = COMMANDS.find((entry) => entry.modes.includes(mode) && entry.match.test(value));
  return command ? { id: command.id, text: value } : null;
}

export function listXiaomeiCommands() {
  return COMMANDS.map(({ id, modes }) => ({ id, modes: [...modes] }));
}
