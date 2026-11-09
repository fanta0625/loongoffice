#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Isolated Linux PDF load checks. Never saves or changes the source PDF."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import traceback
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'uno' / 'python'))


def write(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    temporary.replace(path)


def worker(args):
    import uno
    from edit_bridge import open_for_edit
    from reader_session import ReaderSession
    directory = Path(args.out).resolve()
    started = time.monotonic()
    result = {'mode': args.worker, 'status': 'running', 'phase': 'office-start'}
    def phase(name):
        result.update(phase=name, elapsed_seconds=time.monotonic() - started)
        write(directory / 'worker.json', result)
    pipe = 'pdf_stress_' + uuid.uuid4().hex
    env = os.environ.copy()
    env.pop('LO_PDF_HYBRID_VIEW_PROBE', None)
    if args.worker == 'reader':
        env['LO_IMPORT_USE_PDFIUM'] = '1'
    else:
        env.pop('LO_IMPORT_USE_PDFIUM', None)
    process = None
    document = session = desktop = None
    with (directory / 'office.log').open('w') as log:
        try:
            phase('office-start')
            process = subprocess.Popen([args.office,
                '-env:UserInstallation=' + (directory / 'profile').as_uri(),
                '--headless', '--nologo', '--nodefault', '--norestore', '--nofirststartwizard',
                f'--accept=pipe,name={pipe};urp;StarOffice.ComponentContext'],
                env=env, stdout=log, stderr=subprocess.STDOUT, cwd=directory)
            local = uno.getComponentContext()
            resolver = local.ServiceManager.createInstanceWithContext('com.sun.star.bridge.UnoUrlResolver', local)
            deadline = time.monotonic() + 30
            while True:
                try:
                    context = resolver.resolve(f'uno:pipe,name={pipe};urp;StarOffice.ComponentContext')
                    break
                except Exception:
                    if process.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError('Private Office exited or connection timed out')
                    time.sleep(.1)
            desktop = context.ServiceManager.createInstanceWithContext('com.sun.star.frame.Desktop', context)
            phase('editable-full-import' if args.worker == 'baseline' else 'reader-preview-initialization')
            load_started = time.monotonic()
            if args.worker == 'baseline':
                document = open_for_edit(context, Path(args.pdf).resolve().as_uri(), hidden=True)
            else:
                session = ReaderSession(context, Path(args.pdf).resolve().as_uri(), hidden=True, read_only=True)
                document = session.document
            result.update(pages=document.getDrawPages().getCount(),
                          load_seconds=time.monotonic() - load_started,
                          editable_imports=0 if session else 1)
            phase('loaded')
            result['status'] = 'passed'
        except BaseException as error:
            result.update(status='failed', error_type=type(error).__name__, error=str(error))
            traceback.print_exc()
        finally:
            # Persist before cleanup; a blocked dispose is separately visible.
            write(directory / 'worker.json', result)
            if process:
                result['office_exit_before_cleanup'] = process.poll()
            phase_before_cleanup = result['phase']
            phase('cleanup')
            try:
                if session:
                    session.close()
                elif document:
                    document.dispose()
                if desktop:
                    desktop.terminate()
            except Exception as error:
                result['cleanup_error'] = str(error)
            result['completed_phase'] = phase_before_cleanup
            result['phase'] = 'finished'
            result['elapsed_seconds'] = time.monotonic() - started
            write(directory / 'worker.json', result)
    return 0 if result['status'] == 'passed' else 1


def rss_for_group(group):
    """Sample sum RSS (shared pages may be counted more than once)."""
    total = 0
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if os.getpgid(int(entry.name)) == group:
                for line in (entry / 'status').read_text().splitlines():
                    if line.startswith('VmRSS:'):
                        total += int(line.split()[1])
        except (OSError, ProcessLookupError):
            pass
    return total


def main(args):
    if sys.platform != 'linux':
        raise SystemExit('Run this tool on Linux / WSL with the Office-matched PyUNO environment')
    import resource
    source = Path(args.pdf).resolve(strict=True)
    output = Path(args.out).resolve()
    output.mkdir(parents=True, exist_ok=False)
    with source.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    metadata = {'source': str(source), 'bytes': source.stat().st_size, 'sha256': digest,
                'office': args.office, 'timeout_seconds': args.timeout,
                'per_process_address_space_mib': args.memory_mib,
                'scope': 'Headless full editable import vs reader initialization; not GUI or tile performance',
                'results': []}
    try:
        metadata['office_version'] = subprocess.check_output(
            [args.office, '--version'], stderr=subprocess.STDOUT, timeout=15, text=True).strip()
    except (OSError, subprocess.SubprocessError) as error:
        metadata['office_version_error'] = str(error)
    (output / 'environment.txt').write_text(str(os.uname()) + '\n' + sys.version, encoding='utf-8')
    def limits():
        limit = args.memory_mib * 1024 * 1024
        resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    for mode in ('baseline', 'reader') if args.mode == 'both' else (args.mode,):
        directory = output / mode
        directory.mkdir()
        with (directory / 'worker.log').open('w') as log:
            process = subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                '--worker', mode, '--pdf', str(source), '--office', args.office, '--out', str(directory)],
                stdout=log, stderr=subprocess.STDOUT, start_new_session=True, preexec_fn=limits)
            started, peak, timed_out = time.monotonic(), 0, False
            try:
                while process.poll() is None:
                    peak = max(peak, rss_for_group(process.pid))
                    if time.monotonic() - started > args.timeout:
                        timed_out = True
                        break
                    time.sleep(.2)
            finally:
                # Stop the entire private group, including xpdfimport helpers.
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                    time.sleep(.2)
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
            record = {'mode': mode, 'worker_exit': process.returncode, 'timed_out': timed_out,
                      'sampled_peak_sum_rss_kib': peak, 'elapsed_seconds': time.monotonic() - started}
            if (directory / 'worker.json').exists():
                record['worker'] = json.loads((directory / 'worker.json').read_text())
            record['status'] = ('timeout' if timed_out else
                                'passed' if process.returncode == 0 and record.get('worker', {}).get('status') == 'passed'
                                else 'failed')
            metadata['results'].append(record)
            write(output / 'summary.json', metadata)
            print(json.dumps(record, ensure_ascii=False), flush=True)
    with source.open('rb') as stream:
        metadata['source_unchanged'] = hashlib.file_digest(stream, 'sha256').hexdigest() == digest
    write(output / 'summary.json', metadata)
    return 0 if metadata['source_unchanged'] and all(r['status'] == 'passed' for r in metadata['results']) else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pdf', required=True)
    parser.add_argument('--office', default='libreoffice')
    parser.add_argument('--out', required=True, help='New evidence directory')
    parser.add_argument('--mode', choices=('baseline', 'reader', 'both'), default='both')
    parser.add_argument('--timeout', type=float, default=90)
    parser.add_argument('--memory-mib', type=int, default=4096)
    parser.add_argument('--worker', choices=('baseline', 'reader'), help=argparse.SUPPRESS)
    options = parser.parse_args()
    if options.timeout <= 0 or options.memory_mib < 1024:
        parser.error('timeout must be positive and memory-mib must be at least 1024')
    sys.exit(worker(options) if options.worker else main(options))
