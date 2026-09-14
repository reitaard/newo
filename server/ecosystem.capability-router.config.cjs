const modelDir = process.env.CAPABILITY_ROUTER_MODEL_DIR || "/srv/newo-models/capability-router-v2";

module.exports = {
  apps: [{
    name: "newo-capability-router",
    cwd: "/opt/newo/server",
    script: "capability-router-service.py",
    interpreter: "/srv/newo-capability-router/.venv/bin/python",
    args: `--model-path ${modelDir} --registry config/capability-router.json`,
    exec_mode: "fork",
    instances: 1,
    autorestart: true,
    watch: false,
    max_memory_restart: "768M",
    min_uptime: "10s",
    restart_delay: 3000,
    out_file: "/var/log/newo-capability-router-out.log",
    error_file: "/var/log/newo-capability-router-error.log",
    merge_logs: true,
    time: true,
  }],
};
