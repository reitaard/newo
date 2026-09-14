import { assertConfig, loadConfig } from "./config.js";
import { TavilyProvider } from "./providers/tavily.js";
import { createAgentToolsService } from "./service.js";

const config = loadConfig();
assertConfig(config);
const provider = new TavilyProvider({ apiKey: config.tavilyApiKey, timeoutMs: config.providerTimeoutMs });
const server = createAgentToolsService({ config, provider });

server.listen(config.port, config.host, () => {
  console.info(JSON.stringify({ event: "service_started", service: "agent-tools", host: config.host, port: config.port, provider: provider.name }));
});

function stop(signal) {
  console.info(JSON.stringify({ event: "service_stopping", signal }));
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 10_000).unref();
}
process.once("SIGINT", () => stop("SIGINT"));
process.once("SIGTERM", () => stop("SIGTERM"));
