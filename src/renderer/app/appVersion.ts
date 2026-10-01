declare const __APP_VERSION__: string | undefined;

/** The Clipture version this interface was built as (from package.json). */
export const appVersion: string = typeof __APP_VERSION__ === "string" ? __APP_VERSION__ : "";
