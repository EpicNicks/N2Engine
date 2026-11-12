// Building in GitHub Actions (build targets stage 10, decision D7; docs/apple.html#remote): a target whose hostSystems this
// machine is not (iOS and macOS off a Mac) builds in a workflow of the game's own repository, which runs n2build there.
// n2build starts the workflow with `gh`, follows the run and downloads its artifact into the package folder.
//
//   gitState            the game's repository: its root, the project's path in it, the branch, and whether it is pushed
//   checkPushed         refuses a project with changes the workflow would not see
//   workflowInputs      what the workflow is asked to build
//   dispatchAndFollow   gh workflow run, find the run, gh run watch, gh run download
//   workflowTemplate    the workflow file to put in the game's repository (n2build apple-workflow)
//
// The workflow's inputs are a target, a configuration, a path and an engine revision: nothing secret. Device signing, when
// it comes, uses the repository's own secrets inside the workflow and nothing on this machine.
import path from 'node:path';

import { BuildError } from './report.mjs';

/** The game's git repository, from `git` run in the project folder; run(args) -> {status, stdout} */
export function gitState(projectDir, run) {
  const git = (...args) => run(['-C', projectDir, ...args]);
  const top = git('rev-parse', '--show-toplevel');
  if (!top || top.status !== 0) {
    throw new BuildError(`${projectDir} is not in a git repository: a GitHub Actions build needs the game in a GitHub repository (docs/apple.html#remote)`);
  }
  const root = top.stdout.trim();
  const branch = git('rev-parse', '--abbrev-ref', 'HEAD')?.stdout.trim();
  if (!branch || branch === 'HEAD') throw new BuildError('The game repository is on a detached HEAD: check out a branch, push it, then build');
  const head = git('rev-parse', 'HEAD')?.stdout.trim();
  const upstream = git('rev-parse', '--verify', '--quiet', `origin/${branch}`);
  const dirty = git('status', '--porcelain', '--', '.')?.stdout.trim() ?? '';
  const remote = git('remote', 'get-url', 'origin');
  return {
    root,
    projectPath: path.relative(root, projectDir).split(path.sep).join('/') || '.',
    branch,
    head,
    pushedHead: upstream && upstream.status === 0 ? upstream.stdout.trim() : null,
    dirty: dirty !== '',
    remoteUrl: remote && remote.status === 0 ? remote.stdout.trim() : null,
  };
}

/** The workflow builds what is on GitHub, so the project must be committed and pushed */
export function checkPushed(state) {
  if (!state.remoteUrl || !/github\.com[:/]/.test(state.remoteUrl)) {
    throw new BuildError('The game repository has no GitHub remote named origin: a GitHub Actions build runs in the game\'s GitHub repository');
  }
  if (state.dirty) throw new BuildError('The project has changes that are not committed: commit and push them, so the workflow builds what you see');
  if (state.pushedHead === null) throw new BuildError(`The branch ${state.branch} is not on GitHub yet: push it (git push -u origin ${state.branch})`);
  if (state.pushedHead !== state.head) throw new BuildError(`The branch ${state.branch} has commits that are not pushed: push them first`);
}

/** gh workflow run's -f inputs */
export function workflowInputs({ target, config, projectPath, engineRef }) {
  const inputs = { target, config, project: projectPath };
  if (engineRef) inputs.engine_ref = engineRef;
  return Object.entries(inputs).flatMap(([key, value]) => ['-f', `${key}=${value}`]);
}

/** The newest workflow_dispatch run of the workflow on the branch created at or after `since` (ms); from gh run list --json */
export function pickRun(listJson, since) {
  let runs;
  try {
    runs = JSON.parse(listJson);
  } catch {
    return null;
  }
  const fresh = (Array.isArray(runs) ? runs : [])
    .filter((r) => Date.parse(r.createdAt) >= since - 5000)
    .sort((a, b) => Date.parse(b.createdAt) - Date.parse(a.createdAt));
  return fresh[0] ?? null;
}

/**
 * Start the workflow, follow it to the end, download its artifact into packageDir. exec(args) runs gh to completion and
 * resolves {code, stdout, stderr}; stream(args, onLine) runs gh with its output streamed and resolves its exit code.
 */
export async function dispatchAndFollow({ descriptor, state, config, engineRef, packageDir, reporter, exec, stream, sleep, now = Date.now }) {
  const { workflow, artifact } = descriptor.remote;
  const since = now();
  reporter.step('dispatch', `Starting ${workflow} in GitHub Actions (${descriptor.name}, ${config}, branch ${state.branch})`);
  const started = await exec(['workflow', 'run', workflow, '--ref', state.branch,
    ...workflowInputs({ target: descriptor.name, config, projectPath: state.projectPath, engineRef })]);
  if (started.code !== 0) {
    const reason = (started.stderr || started.stdout).trim();
    throw new BuildError(`gh workflow run ${workflow} failed: ${reason}${/not find|404/i.test(reason) ? ` (is .github/workflows/${workflow} committed and pushed? n2build apple-workflow writes it)` : ''}`);
  }
  // The run shows up in the list a moment after the dispatch
  let run = null;
  for (let i = 0; i < 30 && !run; ++i) {
    await sleep(2000);
    const listed = await exec(['run', 'list', '--workflow', workflow, '--branch', state.branch, '--event', 'workflow_dispatch',
      '--limit', '5', '--json', 'databaseId,createdAt,url']);
    if (listed.code === 0) run = pickRun(listed.stdout, since);
  }
  if (!run) throw new BuildError(`The ${workflow} run did not appear; look at the repository's Actions tab`);
  reporter.step('remote', `Following ${run.url}`);
  const watched = await stream(['run', 'watch', String(run.databaseId), '--exit-status', '--interval', '10'], (line) => reporter.log('stdout', line));
  if (watched !== 0) throw new BuildError(`The GitHub Actions build failed: ${run.url} (gh run view ${run.databaseId} --log-failed)`);
  reporter.step('download', `Downloading the ${artifact} artifact into ${packageDir}`);
  const downloaded = await exec(['run', 'download', String(run.databaseId), '--name', artifact, '--dir', packageDir]);
  if (downloaded.code !== 0) throw new BuildError(`gh run download failed: ${(downloaded.stderr || downloaded.stdout).trim()}`);
  return { run: run.databaseId, url: run.url };
}

/**
 * The workflow for the game's repository. It checks out the game and the engine (the revision n2build asks for), runs
 * n2build for the target on a pinned macOS runner, runs an iOS build in the simulator for 30 seconds, and uploads the
 * package as the artifact n2build downloads.
 */
export function workflowTemplate({ engineRepository = 'EpicNicks/N2Engine' } = {}) {
  return `# Written by n2build apple-workflow (build targets stage 10; docs/apple.html#remote). n2build starts it for the ios and
# macos targets when it runs off a Mac, then downloads the package it uploads. Its inputs are a target, a configuration, the
# project's path and the engine revision: nothing secret. Device signing, when it is added, uses this repository's secrets.
name: n2build-apple

on:
  workflow_dispatch:
    inputs:
      target: { description: 'ios or macos', required: true, type: choice, options: [ios, macos] }
      config: { description: 'debug or release', required: true, default: release, type: choice, options: [debug, release] }
      project: { description: "The project's folder in this repository", required: true, default: '.' }
      engine_ref: { description: 'The N2Engine revision to build with', required: false, default: master }

jobs:
  build:
    runs-on: macos-15
    timeout-minutes: 90
    steps:
      - uses: actions/checkout@v4
        with: { path: game }
      - uses: actions/checkout@v4
        with: { repository: ${engineRepository}, ref: \${{ inputs.engine_ref }}, path: engine }
      - uses: actions/setup-node@v4
        with: { node-version: 22 }
      - name: Install CMake and Ninja
        run: brew install ninja
      - name: n2build doctor
        run: node engine/tools/n2build/n2build.mjs doctor --target "\${{ inputs.target }}" --project "game/\${{ inputs.project }}"
      - name: n2build build
        run: node engine/tools/n2build/n2build.mjs build --target "\${{ inputs.target }}" --project "game/\${{ inputs.project }}" --config "\${{ inputs.config }}" --out "$RUNNER_TEMP/package"
      - name: Run in the simulator (30 seconds)
        if: inputs.target == 'ios'
        run: node engine/tools/n2build/n2build.mjs run --target ios --project "game/\${{ inputs.project }}" --config "\${{ inputs.config }}" --out "$RUNNER_TEMP/package" --no-build --seconds 30
      - uses: actions/upload-artifact@v4
        with: { name: 'n2build-\${{ inputs.target }}', path: '\${{ runner.temp }}/package', include-hidden-files: true }
`;
}
