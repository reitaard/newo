function safeError(error) {
  return {
    error_type: error?.name ?? error?.error?.name ?? "Error",
    error_message: String(error?.message ?? error?.error?.message ?? "unknown").slice(0, 240),
  };
}

export function createTelegramUpdateAcceptor({ handleUpdate, acceptUpdateId, logger,
  schedule = (callback) => queueMicrotask(callback) }) {
  if (typeof handleUpdate !== "function" || typeof acceptUpdateId !== "function") {
    throw new TypeError("handleUpdate and acceptUpdateId are required");
  }

  async function accept(update) {
    const updateId = update?.update_id;
    if (!Number.isSafeInteger(updateId) || updateId < 0) return { accepted: false, invalid: true };
    const firstDelivery = await acceptUpdateId(updateId);
    if (!firstDelivery) {
      logger?.info?.({ telegram_update_id: updateId }, "Ignored duplicate Telegram update");
      return { accepted: true, duplicate: true };
    }
    schedule(() => {
      Promise.resolve(handleUpdate(update)).catch((error) => {
        logger?.error?.({ telegram_update_id: updateId, ...safeError(error) }, "Asynchronous Telegram update failed");
      });
    });
    return { accepted: true, duplicate: false };
  }

  return { accept };
}
