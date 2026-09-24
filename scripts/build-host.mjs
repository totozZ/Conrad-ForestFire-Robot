import { spawnSync } from "node:child_process";
import { existsSync, readdirSync } from "node:fs";
import path from "node:path";

export function cmakePath() {
  if (process.env.CMAKE) return process.env.CMAKE;
  if (process.platform !== "win32") return "cmake";
  const root = `${process.env.ProgramFiles}/Microsoft Visual Studio`;
  if (existsSync(root))
    for (const year of readdirSync(root).sort().reverse()) {
      for (const edition of [
        "Community",
        "BuildTools",
        "Professional",
        "Enterprise",
      ]) {
        const candidate = path.join(
          root,
          year,
          edition,
          "Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe",
        );
        if (existsSync(candidate)) return candidate;
      }
    }
  return "cmake";
}
const cmake = cmakePath();
for (const args of [
  ["-S", ".", "-B", "build/host"],
  ["--build", "build/host", "--config", "Release"],
]) {
  const r = spawnSync(cmake, args, { stdio: "inherit" });
  if (r.error) throw r.error;
  if (r.status) process.exit(r.status);
}
const exe =
  process.platform === "win32"
    ? "build/host/Release/robot_tests.exe"
    : "build/host/robot_tests";
const r = spawnSync(path.resolve(exe), [], { stdio: "inherit" });
if (r.error) throw r.error;
process.exit(r.status ?? 1);
