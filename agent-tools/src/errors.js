export class ServiceError extends Error {
  constructor(status, code, message, details) {
    super(message);
    this.status = status;
    this.code = code;
    this.details = details;
  }
}

export function publicError(error) {
  if (error instanceof ServiceError) return error;
  if (error?.name === "AbortError" || error?.name === "TimeoutError") {
    return new ServiceError(504, "provider_timeout", "The web provider timed out");
  }
  return new ServiceError(502, "provider_error", "The web provider request failed");
}
