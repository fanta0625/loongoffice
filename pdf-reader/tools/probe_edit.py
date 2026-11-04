#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Exercise editable import in a private Office process/profile; no installation."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "uno" / "python"))
from edit_bridge import inspect_filter, open_for_edit


def write_fixture(path):
    content = b"BT /F1 18 Tf 40 120 Td (UNO_PDF_EDIT_PROBE) Tj ET\n40 40 150 30 re S\n"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 200] "
        b"/Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        b"<< /Length " + str(len(content)).encode() + b" >>\nstream\n" + content + b"endstream",
    ]
    data = bytearray(b"%PDF-1.4\n")
    offsets = [0]
    for index, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data.extend(f"{index} 0 obj\n".encode() + obj + b"\nendobj\n")
    xref = len(data)
    data.extend(f"xref\n0 {len(offsets)}\n0000000000 65535 f \n".encode())
    for offset in offsets[1:]:
        data.extend(f"{offset:010d} 00000 n \n".encode())
    data.extend(
        f"trailer\n<< /Size {len(offsets)} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    )
    path.write_bytes(data)


def collect_shapes(container):
    shapes = []
    for index in range(container.getCount()):
        shape = container.getByIndex(index)
        text = shape.getString() if hasattr(shape, "getString") else ""
        shapes.append({"type": shape.getShapeType(), "text": text})
        if shape.supportsService("com.sun.star.drawing.GroupShape"):
            shapes.extend(collect_shapes(shape))
    return shapes


def run(args):
    import uno

    with tempfile.TemporaryDirectory(prefix="pdf-edit-probe-") as tmp:
        directory = Path(tmp)
        fixture = directory / "probe.pdf"
        if args.pdf:
            fixture = Path(args.pdf).resolve(strict=True)
        else:
            write_fixture(fixture)
        pipe = "pdf_edit_probe_" + uuid.uuid4().hex
        command = [args.office, "-env:UserInstallation=" + (directory / "profile").as_uri(),
                   "--headless", "--nologo", "--nodefault", "--nofirststartwizard", "--norestore",
                   f"--accept=pipe,name={pipe};urp;StarOffice.ComponentContext"]
        env = os.environ.copy()
        # Deliberately force the normal load path towards PDFium: direct filter
        # invocation should still use the configured editable import adaptor.
        env["LO_IMPORT_USE_PDFIUM"] = "1"
        with (directory / "office.log").open("w+") as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            desktop = None
            document = None
            try:
                local = uno.getComponentContext()
                resolver = local.ServiceManager.createInstanceWithContext(
                    "com.sun.star.bridge.UnoUrlResolver", local)
                deadline = time.monotonic() + 30
                while True:
                    try:
                        remote = resolver.resolve(
                            f"uno:pipe,name={pipe};urp;StarOffice.ComponentContext")
                        break
                    except Exception:
                        if process.poll() is not None or time.monotonic() >= deadline:
                            raise RuntimeError("Cannot connect to private Office process")
                        time.sleep(0.1)
                desktop = remote.ServiceManager.createInstanceWithContext(
                    "com.sun.star.frame.Desktop", remote)
                _, config = inspect_filter(remote)
                started = time.monotonic()
                document = open_for_edit(remote, fixture.as_uri(), hidden=True)
                pages = document.getDrawPages()
                shapes = []
                for index in range(pages.getCount()):
                    shapes.extend(collect_shapes(pages.getByIndex(index)))
                if not args.pdf:
                    text = "".join(shape["text"] for shape in shapes)
                    if "UNO_PDF_EDIT_PROBE" not in text:
                        raise RuntimeError("Fixture did not become editable Draw text")
                result = {"status": "passed", "mode": "FilterFactory-direct",
                          "office": subprocess.check_output(
                              [args.office, "--version"], text=True).strip(),
                          "filter_service": config["FilterService"],
                          "user_data": list(config["UserData"]), "pages": pages.getCount(),
                          "shapes": shapes, "seconds": time.monotonic() - started,
                          "pdfium_environment_forced": True,
                          "fixture_editable_text_verified": not bool(args.pdf)}
                print(json.dumps(result, indent=2, ensure_ascii=False))
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                try:
                    if document is not None:
                        document.dispose()
                    if desktop is not None:
                        desktop.terminate()
                finally:
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--office", default="libreoffice", help="Office launcher path")
    parser.add_argument("--pdf", help="Optional local PDF instead of generated text/vector fixture")
    run(parser.parse_args())
