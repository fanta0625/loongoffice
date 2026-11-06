#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""P0-A0: build a no-source-change Draw host/layout prototype.

The prototype deliberately uses one tall Draw page. The first and third
panels stand in for reader-owned page previews; the middle panel contains real
Draw text and rectangle objects. This validates Draw's scroll/zoom canvas and
coordinate mapping without claiming that mixed SdPage rendering works.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


def prop(name, value):
    import uno

    p = uno.createUnoStruct("com.sun.star.beans.PropertyValue")
    p.Name = name
    p.Value = value
    return p


def point(x, y):
    import uno

    p = uno.createUnoStruct("com.sun.star.awt.Point")
    p.X, p.Y = x, y
    return p


def size(width, height):
    import uno

    s = uno.createUnoStruct("com.sun.star.awt.Size")
    s.Width, s.Height = width, height
    return s


def add_shape(document, page, service, x, y, width, height, text="", color=0xFFFFFF):
    shape = document.createInstance(service)
    shape.setPosition(point(x, y))
    shape.setSize(size(width, height))
    # Draw text ranges become usable after attaching the shape to its page.
    page.add(shape)
    if hasattr(shape, "FillColor"):
        shape.FillColor = color
        shape.FillTransparence = 0
    if hasattr(shape, "LineColor"):
        shape.LineColor = 0x555555
    if text and hasattr(shape, "String"):
        shape.String = text
        shape.CharHeight = 18
    return shape


def add_panel(document, page, y, title, description, color, editable):
    # Values are 1/100 mm. A tall page makes the normal Draw scroll bar useful.
    add_shape(document, page, "com.sun.star.drawing.RectangleShape",
              1000, y, 18000, 8500, color=color)
    label = add_shape(document, page, "com.sun.star.drawing.TextShape",
                      1600, y + 800, 16800, 1600, title, color)
    label.CharColor = 0x202020
    body = add_shape(document, page, "com.sun.star.drawing.TextShape",
                     1600, y + 3000, 16800, 3000, description, color)
    body.CharColor = 0x303030
    if editable:
        marker = add_shape(document, page, "com.sun.star.drawing.RectangleShape",
                           14500, y + 6200, 3200, 1200, "", 0xA8D8FF)
        marker.String = "EDITABLE"
        marker.CharHeight = 12
        add_shape(document, page, "com.sun.star.drawing.TextShape",
                  2200, y + 6200, 11000, 1000,
                  "真实 Draw 对象：可选择、移动、修改、撤销",
                  color)
    else:
        # This is intentionally a regular shape only as a visual stand-in for
        # reader-owned pixels. A future P0-A1 view hook replaces this area.
        add_shape(document, page, "com.sun.star.drawing.TextShape",
                  2200, y + 6200, 11000, 1000,
                  "阅读核心预览占位：RGBA／图块将在 P0-A1 接入",
                  color)


def make_document(remote, save_url=None):
    services = remote.ServiceManager
    desktop = services.createInstanceWithContext("com.sun.star.frame.Desktop", remote)
    document = desktop.loadComponentFromURL(
        "private:factory/sdraw", "_blank", 0,
        (prop("Hidden", False), prop("ReadOnly", False)),
    )
    if document is None:
        raise RuntimeError("cannot create Draw document")
    page = document.getDrawPages().getByIndex(0)
    page.Width = 20000
    page.Height = 30000
    add_panel(document, page, 1000, "P0-A0 / 页面 1：阅读预览",
              "滚动到这里时，未来由独立 PDF 阅读核心提供页面图像。\n当前仅验证 Draw 画布布局。",
              0xE8F1FF, False)
    add_panel(document, page, 10500, "P0-A0 / 页面 2：按需编辑",
              "这一段包含真实 Draw 文本和图形对象，模拟用户激活当前页编辑后的表示。",
              0xFFF4D6, True)
    add_panel(document, page, 20000, "P0-A0 / 页面 3：阅读预览",
              "页面 3 仍保持阅读表示，不因页面 2 进入编辑而转换。",
              0xE8F1FF, False)
    if save_url:
        document.storeAsURL(save_url, (prop("FilterName", "draw8"),))
    return document


def run(args):
    import uno

    profile = Path(tempfile.mkdtemp(prefix="p0a-draw-host-"))
    pipe = "p0a_draw_host_" + uuid.uuid4().hex
    command = [args.office, f"-env:UserInstallation={profile.as_uri()}",
               "--nologo", "--nodefault", "--nofirststartwizard", "--norestore",
               f"--accept=pipe,name={pipe};urp;StarOffice.ComponentContext"]
    process = subprocess.Popen(command, env=os.environ.copy())
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
                    raise RuntimeError("cannot connect to private Office process")
                time.sleep(0.1)
        save_url = Path(args.save).resolve().as_uri() if args.save else None
        document = make_document(remote, save_url)
        print("P0-A0 Draw host prototype is open.")
        print("Check: vertical scroll, zoom, panel coordinates, and Draw object selection.")
        print("Reader panels are placeholders; this run does not prove mixed SdPage rendering.")
        if args.seconds:
            time.sleep(args.seconds)
        else:
            input("Press Enter after manual validation to close... ")
    finally:
        if document is not None:
            try:
                document.close(True)
            except Exception:
                document.dispose()
        try:
            process.terminate()
            process.wait(timeout=5)
        except Exception:
            process.kill()
            process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--office", default="libreoffice")
    parser.add_argument("--save", help="optional .odg output path")
    parser.add_argument("--seconds", type=int, default=0,
                        help="close after N seconds instead of waiting for Enter")
    run(parser.parse_args())
