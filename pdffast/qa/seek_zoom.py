"""Exercise real desktop PDF navigation/editing on Linux with a private Xvfb.

Run with a Python that provides python3-uno and Pillow. Timings measure UNO
round trips and screenshot stability, not physical display presentation.
Every run owns a fresh profile and input copy; filesystem caches are uncontrolled.
Use the built program on PYTHONPATH and LD_LIBRARY_PATH, with URE_BOOTSTRAP
pointing to its fundamentalrc, to keep the UNO client and server versions aligned.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import threading
import time

import uno
from com.sun.star.beans import PropertyValue
from PIL import ImageGrab


def prop(name, value):
    item = PropertyValue()
    item.Name, item.Value = name, value
    return item


def wait_for(predicate, seconds=90):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.02)
    raise TimeoutError("condition did not complete")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--pdf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--page", type=int, default=400)
    parser.add_argument("--display", default=":94")
    parser.add_argument("--vcl", default="gtk3")
    parser.add_argument("--cpus", help="taskset CPU list for this test's office process")
    parser.add_argument("--stress", action="store_true",
                        help="seek and zoom immediately, without waiting for raster completion")
    parser.add_argument("--preview-only", action="store_true")
    parser.add_argument("--cache-pressure", action="store_true",
                        help="use a 1 MB graphic cache and zero allowed idle seconds in the private profile")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    sample = args.output / "input.pdf"
    shutil.copyfile(args.pdf, sample)
    if args.cache_pressure:
        profile = args.output / "profile" / "user"
        profile.mkdir(parents=True)
        (profile / "registrymodifications.xcu").write_text('''<?xml version="1.0" encoding="UTF-8"?>
<oor:items xmlns:oor="http://openoffice.org/2001/registry">
<item oor:path="/org.openoffice.Office.Common/Cache/GraphicManager">
<prop oor:name="GraphicMemoryLimit" oor:op="fuse"><value>1000000</value></prop>
<prop oor:name="GraphicAllowedIdleTime" oor:op="fuse"><value>0</value></prop>
<prop oor:name="GraphicSwappingEnabled" oor:op="fuse"><value>true</value></prop>
</item>
</oor:items>
''', encoding="utf-8")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = os.environ.copy()
    env.update(DISPLAY=args.display, GDK_BACKEND="x11", SAL_USE_VCLPLUGIN=args.vcl,
               QT_QPA_PLATFORM="xcb",
               SAL_LOG="+INFO.pdffast", SAL_DISABLE_OPENCL="1",
               PATH="/usr/local/bin:/usr/bin:/bin")
    env.pop("WAYLAND_DISPLAY", None)
    display = subprocess.Popen(["Xvfb", args.display, "-screen", "0", "1280x900x24",
                                "-nolisten", "tcp"], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    process = None
    controller = None
    max_ui_roundtrip_ms = 0.0
    watchdog = None
    log = (args.output / "sal.log").open("w")
    metrics = {"page": args.page, "vcl": args.vcl, "cpus": args.cpus,
               "cache_pressure": args.cache_pressure,
               "status": "started", "phases": []}

    def save_metrics():
        (args.output / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")

    def log_text():
        return (args.output / "sal.log").read_text(errors="replace")

    def screenshot():
        return ImageGrab.grab(xdisplay=args.display)

    def settle():
        nonlocal max_ui_roundtrip_ms
        # Observe both sidebar and canvas. Never use this as an exact frame time.
        deadline = time.monotonic() + 15
        previous, stable = None, 0
        while time.monotonic() < deadline:
            image = screenshot()
            # A frozen window also has identical screenshots. Require an office
            # round trip on each observation so synchronous UI rendering cannot
            # masquerade as a completed/stable frame.
            if controller is not None:
                start = time.perf_counter()
                controller.getPropertyValue("ZoomValue")
                max_ui_roundtrip_ms = max(max_ui_roundtrip_ms,
                                         (time.perf_counter() - start) * 1000)
            pixels = image.crop((0, 120, 1100, 840)).tobytes()
            stable = stable + 1 if previous == pixels else 0
            if stable >= 5:
                return image
            previous = pixels
            time.sleep(0.05)
        raise TimeoutError("sidebar/canvas did not settle")

    def phase(name, operation):
        nonlocal max_ui_roundtrip_ms
        max_ui_roundtrip_ms = 0.0
        metrics["active_phase"] = name
        save_metrics()
        print(json.dumps({"begin": name}), flush=True)
        start = time.perf_counter()
        operation()
        api_ms = (time.perf_counter() - start) * 1000
        image = settle()
        elapsed = (time.perf_counter() - start) * 1000
        image.save(args.output / (name + ".png"))
        record = {"name": name, "api_ms": round(api_ms, 1),
                  "screenshot_stable_ms": round(elapsed, 1),
                  "max_ui_roundtrip_ms": round(max_ui_roundtrip_ms, 1)}
        metrics["phases"].append(record)
        save_metrics()
        print(json.dumps(record), flush=True)

    try:
        time.sleep(0.3)
        if display.poll() is not None:
            raise RuntimeError("private display could not start")
        command = [
            str(args.program.resolve() / "soffice"), "--norestore", "--nofirststartwizard",
            "--nologo", "--draw", f'-env:UserInstallation={(args.output / "profile").as_uri()}',
            f"--accept=socket,host=127.0.0.1,port={port};urp;StarOffice.ComponentContext",
            str(sample),
        ]
        if args.cpus:
            command = ["taskset", "-c", args.cpus] + command
        process = subprocess.Popen(command, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)

        def stop_office():
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

        # A blocked UNO call has no Python polling deadline. Terminating this
        # test's process group releases it and allows the finally cleanup.
        watchdog = threading.Timer(120, stop_office)
        watchdog.daemon = True
        watchdog.start()
        context = uno.getComponentContext()
        resolver = context.ServiceManager.createInstanceWithContext(
            "com.sun.star.bridge.UnoUrlResolver", context)

        def connect():
            if process.poll() is not None:
                raise RuntimeError(f"office exited: {process.returncode}")
            try:
                return resolver.resolve(
                    f"uno:socket,host=127.0.0.1,port={port};urp;StarOffice.ComponentContext")
            except Exception:
                return None

        context = wait_for(connect)
        desktop = context.ServiceManager.createInstanceWithContext(
            "com.sun.star.frame.Desktop", context)
        doc = wait_for(lambda: desktop.getCurrentComponent())
        configuration = context.ServiceManager.createInstanceWithContext(
            "com.sun.star.configuration.ConfigurationProvider", context)
        cache = configuration.createInstanceWithArguments(
            "com.sun.star.configuration.ConfigurationAccess",
            (prop("nodepath", "/org.openoffice.Office.Common/Cache/GraphicManager"),))
        metrics["graphic_cache"] = {name: cache.getPropertyValue(name) for name in (
            "GraphicMemoryLimit", "GraphicAllowedIdleTime", "GraphicSwappingEnabled")}
        if args.cache_pressure:
            assert metrics["graphic_cache"] == {
                "GraphicMemoryLimit": 1000000, "GraphicAllowedIdleTime": 0,
                "GraphicSwappingEnabled": True}, metrics["graphic_cache"]
        save_metrics()
        controller = doc.getCurrentController()
        frame = controller.getFrame()
        frame.getContainerWindow().setPosSize(0, 0, 1280, 900, 15)
        wait_for(lambda: "native-ui-image page=0 " in log_text())
        settle().save(args.output / "open.png")
        pages = doc.getDrawPages()
        target = pages.getByIndex(args.page - 1)

        def seek_zoom_stress(prefix):
            # The previous benchmark waited for a settled screenshot after seek,
            # missing cancellation and UI contention while a page was rendering.
            for index in (12, pages.getCount() - 3, 31, pages.getCount() - 22,
                          args.page - 1):
                page = pages.getByIndex(min(max(index, 0), pages.getCount() - 1))

                def navigate():
                    controller.setCurrentPage(page)
                    for zoom in (125, 75, 150, 100, 200, 80):
                        controller.setPropertyValue("ZoomValue", zoom)

                phase(f"{prefix}-seek-zoom-{index + 1}", navigate)

        if args.stress:
            seek_zoom_stress("readonly")
        phase("readonly-seek", lambda: controller.setCurrentPage(target))
        for zoom in (100, 150, 75):
            phase(f"readonly-zoom-{zoom}", lambda z=zoom: controller.setPropertyValue("ZoomValue", z))
        phase("readonly-zoom-burst", lambda: [
            controller.setPropertyValue("ZoomValue", z) for z in range(80, 201, 10)])
        if args.preview_only:
            metrics["status"] = "passed"
            return
        dispatch = context.ServiceManager.createInstanceWithContext(
            "com.sun.star.frame.DispatchHelper", context)
        start = time.perf_counter()
        dispatch.executeDispatch(frame, ".uno:EditDoc", "", 0, (prop("Editable", True),))
        wait_for(lambda: "native-edit-prepare" in log_text() and
                 f"pdf_page={args.page - 1} " in log_text())
        wait_for(lambda: target.getCount() != 1)
        metrics["edit_ready_ms"] = round((time.perf_counter() - start) * 1000, 1)
        metrics["objects"] = target.getCount()
        settle().save(args.output / "edit.png")
        for zoom in (100, 150, 200, 75):
            phase(f"edit-zoom-{zoom}", lambda z=zoom: controller.setPropertyValue("ZoomValue", z))
        phase("edit-zoom-burst", lambda: [
            controller.setPropertyValue("ZoomValue", z) for z in range(80, 201, 10)])
        dispatch.executeDispatch(frame, ".uno:SelectAll", "", 0, ())
        phase("selected-zoom-125", lambda: controller.setPropertyValue("ZoomValue", 125))
        # A second page must also materialize, and returning must reuse the first.
        other = pages.getByIndex(min(args.page, pages.getCount() - 1))
        phase("edit-seek-next", lambda: controller.setCurrentPage(other))
        wait_for(lambda: other.getCount() != 1)
        phase("edit-seek-back", lambda: controller.setCurrentPage(target))
        if args.stress:
            seek_zoom_stress("edit")
        metrics["status"] = "passed"
    except Exception as error:
        metrics.update(status="failed", error=repr(error))
        try:
            screenshot().save(args.output / "failure.png")
        except Exception:
            pass
        raise
    finally:
        if watchdog:
            watchdog.cancel()
        save_metrics()
        if process:
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=10)
            except ProcessLookupError:
                pass
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
        log.close()
        display.terminate()
        display.wait(timeout=5)
    print(json.dumps(metrics), flush=True)


if __name__ == "__main__":
    main()
