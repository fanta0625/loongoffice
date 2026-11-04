# SPDX-License-Identifier: MPL-2.0
import sys
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "uno" / "python"))
import edit_bridge as bridge


class EditBridgeTest(unittest.TestCase):
    def setUp(self):
        self.document = Mock()
        self.document.supportsService.return_value = True
        self.stream = Mock()
        self.importer = Mock()
        self.importer.filter.return_value = True
        self.factory = Mock()
        self.factory.hasByName.return_value = True
        self.factory.getByName.return_value = [
            SimpleNamespace(Name="DocumentService", Value=bridge.DRAW_SERVICE),
            SimpleNamespace(Name="FilterService", Value=bridge.ADAPTOR_SERVICE),
            SimpleNamespace(Name="UserData", Value=(bridge.RAW_IMPORTER, "", bridge.XML_IMPORTER)),
        ]
        self.factory.createInstance.return_value = self.importer
        self.desktop = Mock()
        self.desktop.loadComponentFromURL.return_value = self.document
        files = Mock()
        files.openFileRead.return_value = self.stream
        services = {
            "com.sun.star.document.FilterFactory": self.factory,
            "com.sun.star.frame.Desktop": self.desktop,
            "com.sun.star.ucb.SimpleFileAccess": files,
        }
        self.context = Mock()
        self.context.ServiceManager.createInstanceWithContext.side_effect = (
            lambda name, context: services[name]
        )
        self.properties = patch.object(
            bridge, "property_value", lambda name, value: SimpleNamespace(Name=name, Value=value)
        )
        self.properties.start()
        self.addCleanup(self.properties.stop)

    def test_missing_filter_does_not_create_target(self):
        self.factory.hasByName.return_value = False
        with self.assertRaisesRegex(RuntimeError, "not installed"):
            bridge.open_for_edit(self.context, "file:///original.pdf")
        self.desktop.loadComponentFromURL.assert_not_called()

    def test_unexpected_pipeline_is_rejected_before_import(self):
        self.factory.getByName.return_value[1].Value = "some.other.filter"
        with self.assertRaisesRegex(RuntimeError, "Unexpected PDF filter pipeline"):
            bridge.open_for_edit(self.context, "file:///original.pdf")
        self.factory.createInstance.assert_not_called()

    def test_failed_import_releases_document_and_stream(self):
        self.importer.filter.return_value = False
        with self.assertRaisesRegex(RuntimeError, "returned false"):
            bridge.open_for_edit(self.context, "file:///original.pdf")
        self.document.dispose.assert_called_once()
        self.stream.closeInput.assert_called_once()

    def test_exception_releases_document_and_stream(self):
        self.importer.filter.side_effect = RuntimeError("import failed")
        with self.assertRaisesRegex(RuntimeError, "import failed"):
            bridge.open_for_edit(self.context, "file:///original.pdf")
        self.document.dispose.assert_called_once()
        self.stream.closeInput.assert_called_once()

    def test_success_transfers_document_ownership(self):
        result = bridge.open_for_edit(self.context, "file:///original.pdf", hidden=True)
        self.assertIs(self.document, result)
        self.document.dispose.assert_not_called()
        self.stream.closeInput.assert_called_once()
        self.importer.setTargetDocument.assert_called_once_with(self.document)
        descriptor = {p.Name: p.Value for p in self.importer.filter.call_args.args[0]}
        self.assertEqual("file:///original.pdf", descriptor["URL"])
        self.assertIs(self.stream, descriptor["InputStream"])


if __name__ == "__main__":
    unittest.main()
