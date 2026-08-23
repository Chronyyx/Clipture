const { execSync } = require("node:child_process");

if (process.platform === "win32") {
  try {
    execSync('taskkill /F /IM electron.exe 2>nul', { shell: "cmd.exe", stdio: "ignore" });
  } catch {
    // Process was not running.
  }
  try {
    execSync('taskkill /F /IM clipture_engine.exe 2>nul', { shell: "cmd.exe", stdio: "ignore" });
  } catch {
    // Process was not running.
  }
}
