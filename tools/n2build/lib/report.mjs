// How n2build talks: human lines on stderr/stdout, or (--json) one JSON object per line on stdout for the editor.
//
// JSON events (docs/build-targets.html#json):
//   {"type":"step","id":"configure","title":"..."}            a phase starts
//   {"type":"log","stream":"stdout"|"stderr","text":"..."}    a line from a tool n2build runs
//   {"type":"warning","text":"..."}                           something the user should know, not a failure
//   {"type":"doctor","target":"...","ok":bool,"checks":[...]} a doctor report (also printed before a build)
//   {"type":"targets","targets":[...]}                         the list command
//   {"type":"result","ok":bool,...}                            the last line: the outcome, with "error" when ok is false

export class Reporter {
  /**
   * @param {{json?: boolean, out?: (line: string) => void, err?: (line: string) => void}} options
   */
  constructor({ json = false, out, err } = {}) {
    this.json = json;
    this.out = out ?? ((line) => process.stdout.write(`${line}\n`));
    this.err = err ?? ((line) => process.stderr.write(`${line}\n`));
  }

  emit(event) {
    if (this.json) {
      this.out(JSON.stringify(event));
    }
  }

  step(id, title) {
    if (this.json) this.emit({ type: 'step', id, title });
    else this.err(`==> ${title}`);
  }

  log(stream, text) {
    if (this.json) this.emit({ type: 'log', stream, text });
    else (stream === 'stderr' ? this.err : this.out)(text);
  }

  warning(text) {
    if (this.json) this.emit({ type: 'warning', text });
    else this.err(`warning: ${text}`);
  }

  doctor(report) {
    if (this.json) {
      this.emit({ type: 'doctor', ...report });
      return;
    }
    this.out(`${report.target} (${report.displayName}): ${report.ok ? 'ready' : 'NOT READY'}`);
    for (const check of report.checks) {
      const mark = check.ok ? 'ok ' : check.required ? 'MISSING' : 'absent';
      const version = check.version ? ` ${check.version}` : '';
      this.out(`  [${mark}] ${check.title}${version}${check.detail ? `: ${check.detail}` : ''}`);
      if (!check.ok && check.fix) this.out(`          fix: ${check.fix}`);
    }
  }

  targets(list) {
    if (this.json) {
      this.emit({ type: 'targets', targets: list });
      return;
    }
    for (const t of list) {
      const where = t.buildableHere ? '' : `  (builds on ${t.hostSystems.join(', ')} only)`;
      this.out(`${t.name.padEnd(10)} ${t.displayName}${where}${t.source === 'targets/' ? '' : `  [${t.source}]`}`);
    }
  }

  result(result) {
    if (this.json) {
      this.emit({ type: 'result', ...result });
      return;
    }
    if (result.ok) {
      if (result.output) this.out(`Done: ${result.output}`);
      if (result.archive) this.out(`Archive: ${result.archive}`);
      if (result.url) this.out(`Serving at ${result.url}`);
    } else {
      this.err(`n2build: ${result.error}`);
    }
  }
}

/** A failure with a message meant for the user (no stack trace) */
export class BuildError extends Error {
  constructor(message) {
    super(message);
    this.name = 'BuildError';
  }
}
